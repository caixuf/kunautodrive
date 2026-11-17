/* Host-side contract test for the Win32 message path.
 *
 * A Windows runner is not available in this environment, so compile the header
 * with _WIN32 (no Windows SDK is needed) and assert the capacity contract that
 * makes ipc_channel_publish()'s memcpy into slot->msg.data safe.
 */
#include "message_bus.h"

#include <stdio.h>

_Static_assert(MSG_INLINE_MAX_SIZE == MSG_BUS_MAX_DATA_SIZE,
               "Win32 must use the full 64KB inline bus payload");
_Static_assert(sizeof(((Message*)0)->data) >= MSG_BUS_MAX_DATA_SIZE,
               "Win32 Message.data must hold MSG_BUS_MAX_DATA_SIZE bytes");
_Static_assert(sizeof(Message) >= MSG_BUS_MAX_DATA_SIZE,
               "Win32 ipc slot must fit a full inline message");

int main(void) {
    printf("windows message capacity: inline=%d data=%zu message=%zu: PASS\n",
           MSG_INLINE_MAX_SIZE, sizeof(((Message*)0)->data), sizeof(Message));
    return 0;
}
