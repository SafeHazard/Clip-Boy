#include "sim_flight.h"

#include <math.h>

static SimFlightConfig s_cfg;
static SimFix          s_fix;
static uint32_t        s_t0      = 0;
static bool            s_started = false;

// Metres per degree of latitude. Constant to within a few tenths of a percent
// over the whole ellipsoid, which is far tighter than a simulator needs.
static const double METRES_PER_DEG_LAT = 111320.0;

static const double DEG2RAD = M_PI / 180.0;
static const double RAD2DEG = 180.0 / M_PI;

void sim_flight_default_config(SimFlightConfig* cfg)
{
    if (!cfg) return;
    // Downtown Dallas. Arbitrary, but a real place makes a bad decode obvious:
    // a byte-order slip lands the contact in the ocean.
    cfg->centreLat    = 32.7820;
    cfg->centreLon    = -96.7900;
    // The pilot stands about 800 m south west of the orbit, so the position and
    // the pilot fields differ in the third decimal place and no display can
    // confuse them.
    cfg->operatorLat  = 32.7767;
    cfg->operatorLon  = -96.7970;
    cfg->groundAltGeo = 131.0f;   // roughly field elevation there
    cfg->radiusM      = 150.0f;
    cfg->periodS      = 60.0f;
    cfg->cruiseHeight = 80.0f;
    cfg->climbAmpM    = 25.0f;
    cfg->climbPeriodS = 37.0f;    // deliberately not a factor of periodS
}

void sim_flight_begin(const SimFlightConfig* cfg)
{
    if (!cfg) return;
    s_cfg     = *cfg;
    s_started = false;
    sim_flight_update(0);
}

void sim_flight_update(uint32_t nowMs)
{
    if (!s_started) {
        s_t0      = nowMs;
        s_started = true;
    }
    // Unsigned subtraction, so the millis() rollover at 49 days costs one
    // wrong fix rather than a 49-day jump.
    const double t = (double)(uint32_t)(nowMs - s_t0) / 1000.0;

    const double w     = 2.0 * M_PI / (double)s_cfg.periodS;   // rad/s
    const double theta = w * t;                                // bearing from centre

    const double north = (double)s_cfg.radiusM * cos(theta);
    const double east  = (double)s_cfg.radiusM * sin(theta);

    s_fix.lat = s_cfg.centreLat + north / METRES_PER_DEG_LAT;
    s_fix.lon = s_cfg.centreLon +
                east / (METRES_PER_DEG_LAT * cos(s_cfg.centreLat * DEG2RAD));

    // Velocity is the derivative of the position above: north rate is
    // -Rw sin(theta), east rate is Rw cos(theta). Heading is the compass
    // bearing of that vector, so east over north, not the usual north over east.
    double heading = atan2(cos(theta), -sin(theta)) * RAD2DEG;
    if (heading < 0.0) heading += 360.0;
    s_fix.direction = (float)heading;
    s_fix.speedH    = (float)((double)s_cfg.radiusM * w);

    const double w2 = 2.0 * M_PI / (double)s_cfg.climbPeriodS;
    s_fix.height    = (float)((double)s_cfg.cruiseHeight +
                              (double)s_cfg.climbAmpM * sin(w2 * t));
    s_fix.speedV    = (float)((double)s_cfg.climbAmpM * w2 * cos(w2 * t));
    s_fix.altGeo    = s_cfg.groundAltGeo + s_fix.height;

    // ASTM wants seconds after the full hour relative to UTC. There is no clock
    // on this board, so uptime modulo an hour stands in. It is monotonic and it
    // rolls over the same way, which is all the receiver's timestamp check sees.
    s_fix.timeStamp = (float)fmod(t, 3600.0);
}

const SimFix* sim_flight_fix(void)
{
    return &s_fix;
}

void sim_flight_operator(double* lat, double* lon, float* altGeo)
{
    // The operator stands at the takeoff point and does not wander. This is a
    // different place from the centre of the orbit, on purpose; see the config.
    if (lat)    *lat    = s_cfg.operatorLat;
    if (lon)    *lon    = s_cfg.operatorLon;
    if (altGeo) *altGeo = s_cfg.groundAltGeo;
}
