// A simulated drone flying a lazy circle. Feeds the Remote ID encoder with
// position, altitude and velocity that actually change, so a receiver has
// something to track rather than a single frozen fix.
#pragma once

#include <stdint.h>

// One position report. Units and invalid-value conventions match
// ODID_Location_data so the fields can be copied across without translation.
struct SimFix {
    double lat;
    double lon;
    float  altGeo;     // WGS84 HAE, metres
    float  height;     // above the takeoff point, metres
    float  speedH;     // m/s, positive
    float  speedV;     // m/s, positive is climbing
    float  direction;  // degrees true, 0 <= x < 360
    float  timeStamp;  // seconds after the hour, per ASTM F3411
};

struct SimFlightConfig {
    // Centre of the orbit. Deliberately NOT the operator location: a pilot
    // standing at the centre of the circle reads within a rounding error of the
    // drone on any display, so a receiver showing the drone position in the
    // pilot field would look correct. Keeping them far apart makes that
    // particular bug impossible to miss.
    double centreLat;
    double centreLon;
    double operatorLat;   // where the pilot stands, and the takeoff point
    double operatorLon;
    float  groundAltGeo;  // WGS84 HAE of the takeoff point, metres
    float  radiusM;       // orbit radius
    float  periodS;       // seconds for one full orbit
    float  cruiseHeight;  // mean height above takeoff, metres
    float  climbAmpM;     // half the peak-to-peak altitude wander
    float  climbPeriodS;
};

void          sim_flight_default_config(SimFlightConfig* cfg);
void          sim_flight_begin(const SimFlightConfig* cfg);
void          sim_flight_update(uint32_t nowMs);
const SimFix* sim_flight_fix(void);
void          sim_flight_operator(double* lat, double* lon, float* altGeo);
