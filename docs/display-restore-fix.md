# Windows display restoration patch

This fork repairs the Windows display restoration path used after the last Moonlight session disconnects or its application exits. The existing device preparation, resolution, refresh rate, HDR, revert delay, and revert-on-disconnect settings retain their meanings.

## Failure addressed

The upstream Sunshine restore callback remembered only device IDs and friendly names after a failed restoration. It then skipped every later retry until a display was added or removed. A monitor waking from sleep, a temporary topology failure, or a persistence error can recover while the device list stays identical. The initial empty device list also matched the empty cache and skipped the first restoration attempt.

The Windows display library also switched directly from the streaming topology to the original topology. A failed transition could remove the only verified streaming output. Its fallback attempted an unrelated all-extended layout. The low-level topology setter rejected an empty current topology, preventing recovery when no active display remained.

## Changes

* Retry failures without requiring a hotplug. After the configured initial delay, failed attempts wait 5, 10, 20, 40, then 60 seconds; the final interval repeats. Success stops the timer. Applying a new stream configuration or explicitly resetting persistence cancels pending recovery through the existing serialized scheduler.
* Log the actual restore result and next retry delay. Do not claim displays were enabled unless the display library actually did so.
* Restore original display groups while retaining currently active outputs. Read back the staged topology before removing the streaming-only output, then read back the final topology before clearing the recovery record.
* On failure, retain the recovery record and restore the last verified topology. Preserve clone groups rather than guessing an all-extended desktop.
* Permit the lower Windows API path to reconstruct a requested topology from available paths when the current topology is empty. Require a successful readback; API return status alone is insufficient.

Only the restoration caller uses the additional preservation mode when restoring modified settings. The library's normal topology application keeps its existing behavior.

## Screen saver preparation and accurate outcomes (displayfix2)

A later remote connection failed before display configuration: Windows was using the `Screen-saver` input desktop and the availability check failed. The old launch path continued capturing the unchanged 800x600 VDD. This is distinct from a failed restoration after a stream. A read-only validation probe returned access denied; the original Sunshine log did not retain its native error code.

Windows now performs initial preparation and configuration on a fresh worker and waits for its result before encoder probing. Preparation resets the display idle timer, verifies the session and input desktop, and requests normal closure of a confirmed nonsecure screen saver. It observes the desktop transition with at most 30 polling waits of 50 milliseconds each; native API query time is additional. A scoped attachment changes only the worker's desktop and is restored on that worker after configuration. It does not change screen saver settings, unlock Windows, switch the visible desktop, or terminate a screen saver process.

A positively identified locked session or secure desktop retains the existing remote login path, with configuration explicitly deferred and retried after the user unlocks it. Unknown preparation errors and display-configuration failures return an actionable launch/resume error instead of starting capture with stale settings. Verify-only requests without resolution, refresh-rate, or HDR changes read the requested display's active state without invoking configuration. Failed resumes clean up their preparation just like failed launches. Overlapping pending stream requests are rejected before preparing another display configuration. Expired RTSP requests cancel only their own pending configuration; a newer request is not cancelled by an older timeout.

Native availability failures now record the original Windows error code and message. `NoChangesToRevert` distinguishes an absent recovery record from an actual completed restoration, and is a terminal scheduler result. A no-op is logged as no saved configuration and no display changes, never as restoration completed.

## Reapplying to another release

The accompanying patch bundle contains separate patches for Sunshine and its pinned `third-party/libdisplaydevice` dependency. `Apply-DisplayRestorePatch.ps1` checks both patches before changing either repository, recognizes an already-applied patch, verifies patch hashes, and rejects conflicts. It does not force a patch onto changed upstream code.

`Build-NewVersion.ps1 -Tag latest` obtains the current stable upstream release, initializes its own dependencies, applies the patches, builds the application, and runs the relevant regression tests in that checkout. A specific upstream tag can also be supplied. It does not replace the installed service automatically. Review build/test results and perform a real Moonlight connection/disconnection check before deploying another version.

Future upstream changes may conflict with this patch or already solve the same problem. A rejected patch needs review and porting; compatibility with every future release cannot be guaranteed.

## Validation and limitations

Unit tests inject Windows API errors and incorrect readbacks, and execute the production retry scheduler. These tests establish recovery logic and cancellation behavior. They do not establish physical monitor wake behavior or an external Moonlight stream. Those require live verification on the actual host.

Staged restoration briefly requires the original displays and the streaming output to coexist. If the GPU cannot support that combined topology, restoration remains pending and retains the existing verified output. Unplugged monitors, a failed driver, an inaccessible Windows desktop, or permanently unavailable hardware cannot be made available by a retry policy. The saved state and diagnostics remain available rather than reporting a false success.

Custom binaries are locally built and do not carry the official Sunshine executable signature. The build records the source commit and identifies its publisher as Local Display Revert Fix. Keep the official installation backup for rollback.
