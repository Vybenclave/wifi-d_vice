#pragma once
#include "ui.h"
// Meshtastic mesh monitor over BLE. Acts as a BLE central: pairs with a
// nearby Meshtastic node's GATT API (service 6ba1b218-...), sends the
// want_config handshake, then drains FromRadio and decodes a subset of
// the mesh protobufs -- node list, text messages, device telemetry.
// Receive/display only; nothing is transmitted onto the mesh.
void meshEnter();
void meshLoop();
void meshTouch(const TouchPoint &t);
void meshExit();
bool meshHandleBack();
