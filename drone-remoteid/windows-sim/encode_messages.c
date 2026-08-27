// Generate spec-correct 25-byte Open Drone ID messages for a simulated drone,
// dump each as hex so a Windows BLE script can broadcast them verbatim.
#include <stdio.h>
#include <string.h>
#include "opendroneid.h"

static void dump(const char* name, const void* p) {
    const unsigned char* b = (const unsigned char*)p;
    printf("%s=", name);
    for (int i = 0; i < ODID_MESSAGE_SIZE; i++) printf("%02x", b[i]);
    printf("\n");
}

int main(void) {
    ODID_UAS_Data u;
    odid_initUasData(&u);

    // ----- Basic ID (serial number) -----
    u.BasicID[0].UAType = ODID_UATYPE_HELICOPTER_OR_MULTIROTOR;
    u.BasicID[0].IDType = ODID_IDTYPE_SERIAL_NUMBER;
    strcpy(u.BasicID[0].UASID, "SPACEBADGE-SIM-0001");
    u.BasicIDValid[0] = 1;

    // ----- Location -----
    u.Location.Status = ODID_STATUS_AIRBORNE;
    u.Location.Direction = 90.0f;
    u.Location.SpeedHorizontal = 8.0f;
    u.Location.SpeedVertical = 0.5f;
    u.Location.Latitude = 36.169900;    // Las Vegas-ish (DEF CON vibes)
    u.Location.Longitude = -115.139800;
    u.Location.AltitudeBaro = 620.0f;
    u.Location.AltitudeGeo = 640.0f;
    u.Location.HeightType = ODID_HEIGHT_REF_OVER_TAKEOFF;
    u.Location.Height = 120.0f;
    u.Location.HorizAccuracy = createEnumHorizontalAccuracy(3.0f);
    u.Location.VertAccuracy = createEnumVerticalAccuracy(5.0f);
    u.Location.BaroAccuracy = createEnumVerticalAccuracy(10.0f);
    u.Location.SpeedAccuracy = createEnumSpeedAccuracy(1.0f);
    u.Location.TSAccuracy = createEnumTimestampAccuracy(0.5f);
    u.Location.TimeStamp = 120.0f;
    u.LocationValid = 1;

    // ----- System (operator location) -----
    u.System.OperatorLocationType = ODID_OPERATOR_LOCATION_TYPE_TAKEOFF;
    u.System.ClassificationType = ODID_CLASSIFICATION_TYPE_UNDECLARED;
    u.System.OperatorLatitude = 36.170200;
    u.System.OperatorLongitude = -115.140100;
    u.System.AreaCount = 1;
    u.System.AreaRadius = 0;
    u.System.AreaCeiling = -1000.0f;
    u.System.AreaFloor = -1000.0f;
    u.System.OperatorAltitudeGeo = 610.0f;
    u.System.Timestamp = 0;
    u.SystemValid = 1;

    // ----- Operator ID -----
    u.OperatorID.OperatorIdType = ODID_OPERATOR_ID;
    strcpy(u.OperatorID.OperatorId, "FA-SIM-TEST-0001");
    u.OperatorIDValid = 1;

    // ----- Self ID (free text) -----
    u.SelfID.DescType = ODID_DESC_TYPE_TEXT;
    strcpy(u.SelfID.Desc, "SpaceBadge Test Target");
    u.SelfIDValid = 1;

    ODID_BasicID_encoded    eb;
    ODID_Location_encoded   el;
    ODID_System_encoded     es;
    ODID_OperatorID_encoded eo;
    ODID_SelfID_encoded     ei;

    if (encodeBasicIDMessage(&eb, &u.BasicID[0]) != ODID_SUCCESS) return 1;
    if (encodeLocationMessage(&el, &u.Location) != ODID_SUCCESS) return 2;
    if (encodeSystemMessage(&es, &u.System) != ODID_SUCCESS) return 3;
    if (encodeOperatorIDMessage(&eo, &u.OperatorID) != ODID_SUCCESS) return 4;
    if (encodeSelfIDMessage(&ei, &u.SelfID) != ODID_SUCCESS) return 5;

    dump("BASIC", &eb);
    dump("LOCATION", &el);
    dump("SYSTEM", &es);
    dump("OPERATOR", &eo);
    dump("SELFID", &ei);
    return 0;
}
