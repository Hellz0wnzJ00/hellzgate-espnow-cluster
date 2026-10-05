// the local page and the json behind it
// the master puts up its own access point and serves one page off it. there is
// no router involved and no internet, someone stands next to the unit, joins
// the network and looks at it

#ifndef WEB_H
#define WEB_H

#include "esp_err.h"
#include "tally.h"

// counts come from the background tracker through a locked snapshot.
esp_err_t web_start(void);

// the access point on its own. the station side is untouched, so espnow, the
// scanning and the logging all carry on whichever way this goes
void web_ap_set(int on);
int web_ap_on(void);

#endif
