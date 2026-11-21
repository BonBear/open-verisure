/*
  GuardedSocket.h - socket write with a progress deadline (part of GuardedWebServer).

  The core's NetworkClient::write() retries up to 10 times with a 1 s select() each, so a peer
  that stops reading can block it for ~10 s: longer than the 5 s loop task watchdog. This helper
  waits for writability itself, in short steps, feeds the loop task watchdog only while the peer
  keeps up, and closes the connection when it does not.

  License: GNU Lesser General Public License 2.1 or later (as the Arduino-ESP32 library it is
  used with); written for this project, not copied from the core.
*/

#ifndef GUARDED_SOCKET_H
#define GUARDED_SOCKET_H

#include <Arduino.h>
#include "Network.h"

#define GWS_WRITE_PIECE 1360   // max bytes handed to one send(): fits into the free TCP send buffer when select() says writable

/**
 * @brief Write data to a connected client without ever blocking longer than the limits below.
 * @param client   connected client; stop()ped by this function when a limit is hit.
 * @param stallMs  give up (and stop the client) after this long without any byte accepted.
 * @param totalMs  give up after this long in total; 0 = no total limit.
 * @return bytes written (less than len if the client was dropped or the connection failed).
 *
 * Calls feedLoopWDT() in every step that is still inside the limits, so the caller must run in
 * the (subscribed) loop task. Uses the non-virtual NetworkClient::write() for the pieces, so it
 * can be called from a class that overrides write().
 */
size_t gwsWriteBounded(NetworkClient& client, const uint8_t* data, size_t len, uint32_t stallMs, uint32_t totalMs);

#endif  // GUARDED_SOCKET_H
