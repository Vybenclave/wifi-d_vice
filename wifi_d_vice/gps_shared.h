#pragma once
// Last-known GPS fix, shared with engagement.cpp for its soft time-window
// check. Only updated while the GPS/Wardrive screen has been opened this
// session (GPS is only read there) -- if it's never been opened, there's
// no time reference and callers should treat that as "no fix".
bool gpsGetLastFix(int &year, int &month, int &day, int &hour, int &minute);
