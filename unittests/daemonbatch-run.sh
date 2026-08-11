#!/bin/sh
# G4 count>1 (GetCSMsg::count / torepeat) batch decision regression (issue #4).
#
# iceccd against a fake scheduler; the session activates via ConfCS, a client
# submits GetCS(count=3), and the scheduler answers with three distinct remote
# UseCS.  The daemon must relay exactly three UseCS to the client.
#
# RED before the BATCH_LEDGER: only the first reply is delivered (the 2nd/3rd
# arrive while the client is no longer WAITFORCS and are terminalized as
# unmatched).  GREEN with the ledger: all three are relayed.
dir=$(dirname "$0")
exec "$dir/daemonbatch" "$dir/../daemon/iceccd"
