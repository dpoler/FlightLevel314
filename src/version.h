#pragma once

// This committed value is a placeholder for local/dev builds only -- it
// never matches a real release tag on purpose (an OTA check against it
// always correctly reports "update available", never "up to date" by
// accident). Real releases get the actual version stamped in here by
// .github/workflows/release.yml (overwrites this file with the pushed git
// tag right before building), matching how dpoler/FlightRadarCYD -- the
// project this OTA feature was adapted from -- already does it. Don't hand-
// bump this for a release; push a tag and let the workflow do it, so the
// binary and the tag it's attached to can never drift apart.
#define FIRMWARE_VERSION_STR "v0.0.0-dev"
