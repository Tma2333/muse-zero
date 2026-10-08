#pragma once
/* Build flavor switch (C14 release qualification).
 * 0 = development build (the committed default): FAKE_RUN dev op is
 *     dispatched and advertised, and the 90 s auto-exit safety timer
 *     is active.
 * 1 = release build: FAKE_RUN is refused like any unsupported op and
 *     omitted from GET_CAPABILITIES, and there is no auto-exit (local
 *     Back exit is the lifecycle).
 * tools/make_release.sh flips this to 1 for the release artifact and
 * restores 0 afterwards. Never commit the flipped state. */
#define MB_RELEASE_BUILD 0
