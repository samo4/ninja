# Deep Sleep Implementation Plan (System ON + RTC)

## Goal

Achieve **sub-10 µA sleep** current on the nRF9160 in the Empty personality
(and eventually all personalities) while retaining:

- LIS2DTW12 motion wake-up via P0.30 (INT1)
- Periodic RTC wake-up (e.g. once per week for a keep-alive POST)

---

## 1. Sleep Mode Comparison

| Feature                 | **System OFF**                                  | **System ON + RTC** (★ chosen)                    | **System ON, HFCLK active** (current) |
| ----------------------- | ----------------------------------------------- | ------------------------------------------------- | ------------------------------------- |
| **Typical current**     | ~0.6 µA                                         | **~2–5 µA**                                       | ~30–300 µA                            |
| **Wake-up sources**     | GPIO SENSE only                                 | RTC, GPIO/GPIOTE, LPCOMP, UART, SPI, I2C, NFC     | Everything                            |
| **Periodic timer**      | ❌ (RTC powered down)                           | ✅ **RTC compare**                                | ✅                                    |
| **Motion wake (P0.30)** | ✅ (SENSE only, complex)                        | ✅ **GPIOTE** (simple, already works)             | ✅                                    |
| **Reboot on wake**      | ✅ (full POR)                                   | ❌ **Resumes code**                               | ❌                                    |
| **State retention**     | ❌ (SRAM lost)                                  | ✅ **SRAM retained**                              | ✅                                    |
| **Code complexity**     | SENSE, open-drain, SPI clean, TF-M coordination | standard GPIO, no ping reconfig, no secure issues | Zero (no sleep)                       |
| **Peripheral re-init**  | Everything (full boot)                          | Nothing needed                                    | Nothing needed                        |
| **TF-M interaction**    | Must coordinate with secure side                | **None** (non-secure only)                        | None                                  |
| **Real-time clock**     | Lost                                            | **Preserved**                                     | Preserved                             |

### Why Option B (System ON + RTC)

- Still **well under 10 µA** when implemented correctly
- **Dramatically simpler** — no GPIO SENSE, no open-drain, no SPI pin release, no TF-M coordination
- **Resumes code** — no full reboot, no modem re-init needed (the modem is already off via `lte_lc_offline()`)
- **RTC periodic wake** is built-in, no external hardware needed
- The motion module's existing GPIOTE interrupt already works for wake

---

## 2. What System ON + WFI Actually Means

In System ON idle, the CPU executes `__WFI()` (Wait For Interrupt) when the idle
thread runs. The key to reaching ~2–5 µA:

1. **All application threads blocked** (waiting for zbus messages, timers, etc.)
2. **HFCLK stopped** — no peripheral actively requesting the high-frequency clock
3. **Only low-power peripherals running:** RTC (always on), GPIO/GPIOTE for wake
4. **CPU in WFI** — powered down until an interrupt fires

The challenge is identifying what's keeping HFCLK running and systematically
stopping it.

---

## 3. Current State Audit

### 3.1 What Already Exists

| Area                                     | Status                                                                 | Notes                             |
| ---------------------------------------- | ---------------------------------------------------------------------- | --------------------------------- |
| `CONFIG_PM=y`                            | ✅ `prj.conf`                                                          | Enables Zephyr power management   |
| `CONFIG_PM_DEVICE=y`                     | ✅ `prj.conf`                                                          | Enables device PM                 |
| `CONFIG_PM_DEVICE_RUNTIME=y`             | ✅ `prj.conf`                                                          | Auto-suspends unused peripherals  |
| Modem offline before sleep               | ✅ `lte_lc_offline()` called in `network_disconnect()`                 | Goes to `STATE_DISCONNECTED_IDLE` |
| GPIOTE for motion wake                   | ✅ Already works                                                       | `GPIO_INT_LEVEL_HIGH` on P0.30    |
| Main thread blocks on zbus               | ✅ `zbus_sub_wait_msg()`                                               | Thread is idle when no messages   |
| Motion thread blocks on zbus             | ✅ Same pattern                                                        | Thread is idle                    |
| Network thread blocks on zbus            | ✅ Same pattern                                                        | Thread is idle                    |
| `sys_power_state_set(PM_STATE_SOFT_OFF)` | ❌ called but never reaches idle — **dead code pattern**               |
| GPIO SENSE (low-power wake-up)           | ❌ not used — GPIOTE is active instead                                 |
| LIS2DTW12 INT pin open-drain             | ❌ default push-pull will leak into nRF9160 in System OFF              |
| Modem fully shut down before sleep       | ❌ `lte_lc_offline()` only, modem library still initialized            |
| SPI pins released before sleep           | ❌ `low-power-enable` in pinctrl helps in System ON but not System OFF |
| All threads stopped before sleep         | ❌ network, motion, main threads still alive                           |

### 3.2 Why ~40 µA Floor Currently

The ~40 µA is the nRF9160 with HFCLK running (active System ON). Suspects:

1. **GPIOTE** — configured and armed for P0.30 interrupt. GPIOTE keeps HFCLK
   running while armed → ~30–40 µA.
   - **But** GPIOTE is needed for wake — this may be fine, but needs
     verification that GPIOTE in "wait for event" mode doesn't force HFCLK.

2. **Modem library initialized** — `nrf_modem_lib_init()` was called. Even after
   `lte_lc_offline()`, the modem library holds internal resources.
   - **Fix:** `nrf_modem_lib_shutdown()` before sleep.

3. **SPI2** — `spi2` is enabled and may keep its peripheral clock running even
   when idle, depending on `CONFIG_PM_DEVICE_RUNTIME` behaviour.
   - **Fix:** Ensure the LIS2DTW12 driver properly suspends its SPI bus.

4. **PWM0** — `pwm0` is configured (`pinctrl-0`), `CONFIG_PWM=y`. If the PWM
   peripheral is enabled but idle, it may keep HFCLK alive.
   - **Fix:** Ensure `pwm0` is suspended when not in use.

5. **RTT / UART** — `CONFIG_RTT_CONSOLE=y`. RTT uses the debugger's JTAG/SWD
   connection. If the J-Link is connected, it may prevent deep sleep.
   - **For measurement:** Disconnect J-Link after flashing.

6. **System workqueue / timers** — If any periodic timers are running
   (watchdog, heartbeat, etc.), they keep the scheduler active and prevent WFI.
   - **Fix:** Stop heartbeat timer before sleep. Delete task WDT entries.

---

## 4. Implementation Plan

### Phase 1: Clean Up the Personality

#### [x] 1.1 Remove dead PM code from `empty.c` `sleeping_entry()` (done)

#### [ ] 1.2 Add a "ready to sleep" signal from personality to main

**Approach:** Add a flag that main checks after `personality_process()`.

In `empty.h`:

```c
bool personality_is_sleeping(void);
```

In `empty.c`:

```c
static bool sleep_requested;

bool personality_is_sleeping(void) { return sleep_requested; }
```

Set `sleep_requested = true` in `sleeping_entry`.

#### [ ] 1.3 Cancel fallback timer on entering sleep

In `empty.c` `sleeping_entry()`, cancel the `sample_timer_work`:

```c
k_work_cancel_delayable(&sample_timer_work);
```

Otherwise the 600-second timer fires and wakes the CPU for nothing.

#### [ ] 1.4 Subscribe to motion_chan for wake-up handling

**In `empty.h`:** Add `motion_chan` to `EMPTY_CHANNEL_LIST`:

```c
#define EMPTY_CHANNEL_LIST(X)                 \
    X(timer_chan, struct empty_timer_msg)     \
    X(cloud_post_chan, struct cloud_post_msg) \
    X(network_chan, struct network_msg)       \
    X(motion_chan, struct motion_msg)         // NEW
```

**In `empty.c` `sleeping_run()`:** Handle wake events:

```c
static enum smf_state_result sleeping_run(void *o) {
    struct empty_state_object *state = (struct empty_state_object *)o;

    // Wake from motion sensor — restart the cycle
    if (state->chan == &motion_chan) {
        LOG_INF("Motion wake-up, reconnecting");
        sleep_requested = false;
        smf_set_state(SMF_CTX(state), &states[EMPTY_STATE_WAITING_MODULE]);
        return SMF_EVENT_HANDLED;
    }

    // Wake from RTC periodic timer — restart the cycle
    if (state->chan == &timer_chan) {
        LOG_INF("Periodic RTC wake-up, reconnecting");
        sleep_requested = false;
        smf_set_state(SMF_CTX(state), &states[EMPTY_STATE_WAITING_MODULE]);
        return SMF_EVENT_HANDLED;
    }

    return SMF_EVENT_PROPAGATE;
}
```

---

### Phase 2: Main Thread Orchestration

#### [ ] 2.1 Main thread checks for sleep request

**In `main.c`:**

```c
#include "common/sleep.h"

// ...

while (1) {
    TASK_WDT_FEED();
    err = zbus_sub_wait_msg(&main_subscriber, &state.chan,
                            state.msg_buf, zbus_wait_ms);
    if (err == -ENOMSG) continue;
    else if (err) { SEND_FATAL_ERROR(); return; }

    personality_process(&state);

    if (personality_is_sleeping()) {
        enter_wfi_sleep();
        // On wake from WFI, execution returns here
        // Re-init modules that were suspended
        wake_init();
    }
}
```

#### [ ] 2.2 The `enter_wfi_sleep()` function

**New file:** `src/common/sleep.c`

```c
#include <zephyr/logging/log.h>
#include <nrf_modem.h>
#include "sleep.h"
#include "motion.h"
#include "network.h"
#include "heartbeat.h"

void enter_wfi_sleep(void) {
    LOG_INF("Entering deep System ON sleep...");

    // Step 1: Stop the heartbeat
    heartbeat_stop();

    // Step 2: Cancel all personality timers
    // (handled in Phase 1.3 — cancel in sleeping_entry)

    // Step 3: Shut down the modem library fully
    // (modem is already in offline mode from network_disconnect)
    nrf_modem_lib_shutdown();

    // Step 4: Suspend motion module
    // (disable P0.30 interrupt, put LIS2DTW12 into power-down)
    motion_suspend_for_sleep();

    // Step 5: Delete task WDT entries (so they don't fire during sleep)
    network_suspend_wdt();
    motion_suspend_wdt();

    // Step 6: Flush pending logs
    LOG_PANIC();

    // Step 7: Return — the idle thread will now run → WFI → ~2-5 µA
    // Any interrupt (motion on P0.30, RTC timer) wakes the CPU.
    LOG_INF("Sleeping: motion or periodic timer will wake the device");
}
```

**Key difference from System OFF:** This function does NOT call
`sys_power_state_set(PM_STATE_SOOT_OFF)`. It just sets up the
conditions for deep idle and returns. The actual WFI happens
naturally in the idle thread.

#### [ ] 2.3 The `wake_init()` function

```c
void wake_init(void) {
    LOG_INF("Woke from deep sleep, re-initializing...");

    // Step 1: Re-init the modem library
    nrf_modem_lib_init();

    // Step 2: Resume motion module (wake LIS2DTW12, re-arm GPIO interrupt)
    motion_resume_after_wake();

    // Step 3: Re-arm WDT entries
    network_resume_wdt();
    motion_resume_wdt();

    // Step 4: Restart heartbeat
    heartbeat_start();

    LOG_INF("Wake init complete");
}
```

---

### Phase 3: Motion Module Updates

#### [ ] 3.1 Add suspend/resume functions

**File:** `src/modules/motion/motion.h`

```c
int motion_suspend_for_sleep(void);
int motion_resume_after_wake(void);
void motion_suspend_wdt(void);
void motion_resume_wdt(void);
```

**File:** `src/modules/motion/motion.c`

`motion_suspend_for_sleep()`:

```c
int motion_suspend_for_sleep(void) {
    int err;

    // 1. Disable GPIO interrupt
    err = gpio_pin_interrupt_configure_dt(&motion_int, GPIO_INT_DISABLE);
    if (err) {
        LOG_ERR("Failed to disable motion interrupt: %d", err);
        return err;
    }

    // 2. Remove GPIO callback (so GPIOTE is fully released)
    gpio_remove_callback(motion_int.port, &motion_int_cb_data);

    // 3. Put LIS2DTW12 into power-down mode (~1 µA)
    //    Write 0x00 to CTRL1 (register 0x20)
    uint8_t ctrl1_val = 0;
    err = lis2dtw12_write_reg(driver_ctx, LIS2DTW12_CTRL1, &ctrl1_val, 1);
    if (err) {
        LOG_ERR("Failed to put LIS2DTW12 into power-down: %d", err);
        return err;
    }

    LOG_INF("Motion module suspended for sleep");
    return 0;
}
```

`motion_resume_after_wake()`:

```c
int motion_resume_after_wake(void) {
    int err;

    // 1. Re-init the sensor (wake from power-down)
    //    Re-write the ODR and configuration
    err = configure_sensor();
    if (err) return err;

    // 2. Re-configure the GPIO interrupt
    err = configure_interrupt();
    if (err) return err;

    LOG_INF("Motion module resumed after wake");
    return 0;
}
```

`motion_suspend_wdt()` / `motion_resume_wdt()`:
Store the `task_wdt_id` (currently a local) as a module-level variable,
then:

```c
void motion_suspend_wdt(void) {
    if (motion_task_wdt_id >= 0) {
        task_wdt_delete(motion_task_wdt_id);
        motion_task_wdt_id = -1;
    }
}

void motion_resume_wdt(void) {
    motion_task_wdt_id = task_wdt_add(wdt_timeout_ms,
                                      mot_wdt_callback,
                                      (void *)k_current_get());
}
```

**Note:** This requires refactoring the motion thread to store
`task_wdt_id` at module scope instead of a local.

---

### Phase 4: Network Module Updates

#### [ ] 4.1 Add suspend/resume functions

**File:** `src/modules/network/network.h`

```c
void network_suspend_wdt(void);
void network_resume_wdt(void);
```

Same WDT pattern as motion module.

#### [ ] 4.2 Modem library re-init on wake

The network module's `state_running_entry` already calls
`nrf_modem_lib_init()`. However, after `nrf_modem_lib_shutdown()`,
the next `nrf_modem_lib_init()` should work fine (it re-initializes
the modem library).

But there's a sequencing issue: `wake_init()` in `sleep.c` calls
`nrf_modem_lib_init()` before the network module thread runs. The
network module also calls `nrf_modem_lib_init()` in `state_running_entry`.

**Fix:** Track the modem library init state or ensure `wake_init()`
runs `nrf_modem_lib_init()` and the network module checks if it's
already initialized. Or simply remove it from `wake_init()` and let
the network module handle it when the personality requests a reconnect.

**Simpler approach:** Don't call `nrf_modem_lib_init()` in `wake_init()`.
Instead, the network module's `state_running_entry` will re-init it
automatically when the personality publishes `NETWORK_CONNECT` and the
state machine transitions through `STATE_RUNNING`.

But wait — after `nrf_modem_lib_shutdown()`, the network module is in
`STATE_DISCONNECTED_IDLE`. The personality publishes `NETWORK_CONNECT`,
which transitions to `STATE_DISCONNECTED_SEARCHING`. But the modem
library was shut down — `lte_lc_connect_async()` will fail.

**Fix:** The network module's transition from `DISCONNECTED_IDLE` to
`DISCONNECTED_SEARCHING` must first call `nrf_modem_lib_init()`. Or,
detect the shutdown state and re-init.

**Cleanest approach:** Add a `network_resume()` function that:

1. Calls `nrf_modem_lib_init()` if needed
2. Re-registers the LTE handler
3. Then the personality sends `NETWORK_CONNECT` as normal

---

### Phase 5: RTC Periodic Wake (k_timer approach)

#### [ ] 5.1 Add a sleep timer to the personality

When entering SLEEPING state, arm a `k_work_delayable` for the
periodic wake interval:

```c
// In empty.h or prj.conf
#define PERIODIC_WAKE_SECONDS 604800  // 7 days

// In sleeping_entry():
k_work_reschedule(&sample_timer_work, K_SECONDS(PERIODIC_WAKE_SECONDS));
```

This timer fires after 7 days. The idle thread processes expired
timers and wakes the CPU. The timer message is delivered to the
personality's `sleeping_run()`, which handles it (Phase 1.4).

**Note:** `k_timer` / `k_work_delayable` uses the system RTC. The
maximum timeout for `K_SECONDS()` is ~2^31 ticks, which at 32.768 kHz
is about 18 hours. For 7 days, you need to use a chained approach:

```c
#define WAKE_INTERVAL_HOURS 6  // 6 hours — max safe for k_timeout_t
// Re-arm on each wake until 7 days have passed
```

Or use the RTC HAL directly with compare registers for the full 7-day
interval. This needs investigation.

#### [ ] 5.2 Kconfig for wake interval

In `prj.conf` or Kconfig:

```
# Periodic wake interval (seconds). 0 = disabled
CONFIG_APP_SLEEP_PERIODIC_WAKE_SECONDS=604800
```

---

### Phase 6: Heartbeat and Watchdog

#### [ ] 6.1 Add heartbeat stop/start

**File:** `src/common/heartbeat.h`

```c
void heartbeat_stop(void);
void heartbeat_start(void);
```

**File:** `src/common/heartbeat.c` — implement by cancelling/restarting
the heartbeat work timer.

#### [ ] 6.2 Task WDT handling

Each module stores its `task_wdt_id` at module scope and provides
`_suspend_wdt()` / `_resume_wdt()` functions (see Phases 3–4).

---

### Phase 7: Kconfig Tuning

#### [ ] 7.1 Ensure these are set

```
CONFIG_PM=y               # already set
CONFIG_PM_DEVICE=y         # already set
CONFIG_PM_DEVICE_RUNTIME=y # already set
CONFIG_TICKLESS_IDLE=y     # default in Zephyr
```

#### [ ] 7.2 Reduce debug logging for sleep path

```
CONFIG_APP_EMPTY_LOG_LEVEL_WRN=y  # less logging = fewer timer wake-ups
```

#### [ ] 7.3 Consider `CONFIG_PM_NEED_ALL_DEVICES_SUSPENDED=y`

Forces all devices to suspend before the system can enter deep idle.
May help ensure SPI/PWM are truly powered down.

---

### Phase 8: GPIOTE and HFCLK Investigation

#### [ ] 8.1 Measure whether GPIOTE keeps HFCLK alive

This is the critical unknown. Two possibilities:

1. **GPIOTE with armed interrupt keeps HFCLK running** → need to
   switch to PORT event (DETECT signal) for sleep phase
2. **GPIOTE can be in "wait" mode without HFCLK** → no change needed

**If GPIOTE is the problem:** The fix is to temporarily reconfigure
P0.30 to use the GPIO PORT event instead of GPIOTE during sleep.
On the nRF9160, the PORT event is a low-power alternative that
doesn't require HFCLK — it's essentially the same as SENSE but
accessible through the standard GPIO driver API.

**How to test:** Build the current code, enter sleep state, measure
current. If still ~40 µA, GPIOTE is likely the culprit.

**Fallback if needed:** Before sleep:

1. Disable GPIOTE interrupt on P0.30
2. Re-configure the pin using the PORT mechanism
3. This is simpler than full SENSE config from the old System OFF plan

---

## 5. Files to Create / Modify

### New Files

| File                 | Purpose                                         |
| -------------------- | ----------------------------------------------- |
| `src/common/sleep.h` | `enter_wfi_sleep()`, `wake_init()` declarations |
| `src/common/sleep.c` | Implementation of sleep orchestration           |

### Modified Files

| File                            | Changes                                                                                     |
| ------------------------------- | ------------------------------------------------------------------------------------------- |
| `src/personalities/empty.h`     | Add `personality_is_sleeping()`, add `motion_chan` to channel list                          |
| `src/personalities/empty.c`     | Add `sleep_requested` flag, handle motion/RTC wake in `sleeping_run`, cancel fallback timer |
| `src/main.c`                    | Check `personality_is_sleeping()`, call `enter_wfi_sleep()`                                 |
| `src/modules/motion/motion.h`   | Add suspend/resume/WDT functions                                                            |
| `src/modules/motion/motion.c`   | Implement suspend/resume, store `task_wdt_id` at module scope                               |
| `src/modules/network/network.h` | Add WDT suspend/resume functions, add `network_resume()`                                    |
| `src/modules/network/network.c` | Implement, store `task_wdt_id` at module scope                                              |
| `src/common/heartbeat.h`        | Add `heartbeat_stop()`, `heartbeat_start()`                                                 |
| `src/common/heartbeat.c`        | Implement stop/start                                                                        |
| `prj.conf`                      | Add periodic wake interval config, tune PM settings                                         |

---

## 6. Testing Checklist

- [ ] Build passes after all changes
- [ ] Device reaches ~2–5 µA in sleep (disconnect J-Link for true measurement)
- [ ] Motion (LIS2DTW12 INT1) wakes the device → reconnects LTE → sends POST
- [ ] Periodic timer wakes the device at the configured interval → reconnects → sends POST
- [ ] No spurious wake-ups (verify with RTT log that only expected wake sources fire)
- [ ] Modem re-initializes correctly after wake
- [ ] LIS2DTW12 re-initializes correctly after wake (power-down → power-up)
- [ ] Multiple wake-sleep cycles work without memory leaks or state corruption
- [ ] WDT correctly disabled during sleep, re-enabled on wake
- [ ] Heartbeat suppressed during sleep, resumes on wake
- [ ] GPIOTE does not prevent deep idle (measure current with/without J-Link)
- [ ] PWM0 does not keep HFCLK active when idle

---

## 7. Open Questions

1. **Does GPIOTE with an armed interrupt force HFCLK to stay on?**
   - If yes → need PORT event fallback (see Phase 8)
   - Testing with a DMM after entering sleep will confirm

2. **`k_timeout_t` max value for 7-day timer?**
   - `K_SECONDS(604800)` may overflow internal tick math (~18 h limit)
   - May need chained timers or RTC HAL compare registers

3. **`nrf_modem_lib_shutdown()` + re-init sequencing?**
   - Does the network module handle re-init gracefully?
   - Need to test: shutdown → wake → connect → works?

4. **Does the LIS2DTW12 SPI bus properly suspend via `CONFIG_PM_DEVICE_RUNTIME`?**
   - After sensor goes to power-down and no more SPI transactions,
     the SPI peripheral should clock-gate automatically
   - Verify with current measurement

5. **Race condition: motion fires during shutdown sequence?**
   - `enter_wfi_sleep()` disables motion interrupt first, then proceeds
   - If motion fires between entering sleeping_entry and disabling the
     interrupt, the message is published but no one handles it
   - Mitigation: disable interrupt first, then proceed with other steps
