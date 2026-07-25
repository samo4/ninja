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
| **Motion wake (P0.30)** | ✅ (SENSE only, complex)                        | ✅ **SENSE + PORT event** (simple, already works) | ✅                                    |
| **Reboot on wake**      | ✅ (full POR)                                   | ❌ **Resumes code**                               | ❌                                    |
| **State retention**     | ❌ (SRAM lost)                                  | ✅ **SRAM retained**                              | ✅                                    |
| **Code complexity**     | SENSE, open-drain, SPI clean, TF-M coordination | standard GPIO, no ping reconfig, no secure issues | Zero (no sleep)                       |
| **Peripheral re-init**  | Everything (full boot)                          | Nothing needed                                    | Nothing needed                        |
| **TF-M interaction**    | Must coordinate with secure side                | **None** (non-secure only)                        | None                                  |
| **Real-time clock**     | Lost                                            | **Preserved**                                     | Preserved                             |

### Why Option B (System ON + RTC)

- Still **well under 10 µA** when implemented correctly
- **Dramatically simpler** — no GPIO SENSE, no open-drain, no SPI pin release, no TF-M coordination
- **Resumes code** — no full reboot, no modem re-init needed (the modem is fully powered off via `lte_lc_power_off()` before sleep)
- **RTC periodic wake** is built-in, no external hardware needed
- The motion module's existing `GPIO_INT_LEVEL_HIGH` already uses **SENSE + PORT event** (no GPIOTE channel) — it works for both active and sleep modes

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

| Area                                       | Status                                                                                                                   | Notes                                                                                                    |
| ------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------- |
| `CONFIG_PM=y`                              | ✅ `prj.conf`                                                                                                            | Enables Zephyr power management                                                                          |
| `CONFIG_PM_DEVICE=y`                       | ✅ `prj.conf`                                                                                                            | Enables device PM                                                                                        |
| `CONFIG_PM_DEVICE_RUNTIME=y`               | ✅ `prj.conf`                                                                                                            | Auto-suspends unused peripherals                                                                         |
| Modem set to offline (RF off) before sleep | ✅ `lte_lc_offline()` called in `network_disconnect()` — RF off, but modem still powered                                 | Goes to `STATE_DISCONNECTED_IDLE` — insufficient alone, need `lte_lc_power_off()` in `enter_wfi_sleep()` |
| GPIOTE for motion wake                     | ✅ Already works                                                                                                         | `GPIO_INT_LEVEL_HIGH` on P0.30 — uses SENSE+PORT, not GPIOTE channel                                     |
| Main thread blocks on zbus                 | ✅ `zbus_sub_wait_msg()`                                                                                                 | Thread is idle when no messages                                                                          |
| Motion thread blocks on zbus               | ✅ Same pattern                                                                                                          | Thread is idle                                                                                           |
| Network thread blocks on zbus              | ✅ Same pattern                                                                                                          | Thread is idle                                                                                           |
| `sys_power_state_set(PM_STATE_SOFT_OFF)`   | ❌ called but never reaches idle — **dead code pattern**                                                                 |
| GPIO SENSE (low-power wake-up)             | ✅ **Already active** — `GPIO_INT_LEVEL_HIGH` on nRF9160 sets SENSE in PIN_CNF and uses PORT event, not a GPIOTE channel |
| LIS2DTW12 INT pin open-drain               | ❌ default push-pull will leak into nRF9160 in System OFF                                                                |
| Modem fully shut down before sleep         | ❌ `lte_lc_offline()` only, modem library still initialized                                                              |
| SPI pins released before sleep             | ❌ `low-power-enable` in pinctrl helps in System ON but not System OFF                                                   |
| All threads stopped before sleep           | ❌ network, motion, main threads still alive                                                                             |

### 3.2 Why ~40 µA Floor Currently

The ~40 µA is the nRF9160 with HFCLK running (active System ON). Suspects:

1. ~~GPIOTE~~ — **RESOLVED: Not the culprit.** `GPIO_INT_LEVEL_HIGH` on nRF9160
   does **not** allocate a GPIOTE channel. The nRF GPIO driver uses only the
   **SENSE** field in PIN_CNF + the **PORT event** for level triggers. No
   GPIOTE channel means no HFCLK is kept running. The motion interrupt is
   already configured in the lowest-power way possible for System ON sleep.

2. **Modem library initialized** — `nrf_modem_lib_init()` was called. Even after
   `lte_lc_power_off()`, the modem library holds internal resources.
   - **Fix:** `lte_lc_power_off()` before sleep (called from `enter_wfi_sleep()`).

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
#include "sleep.h"
#include "network.h"
#include "motion.h"
#include "heartbeat.h"

void enter_wfi_sleep(void) {
    LOG_INF("Entering deep System ON sleep...");

    // Step 1: Stop the heartbeat
    heartbeat_stop();

    // Step 2: Cancel all personality timers
    // (handled in Phase 1.3 — cancel in sleeping_entry)

    // Step 3: Fully power off the modem
    lte_lc_power_off();

    // Step 4: Suspend task WDT entries (so they don't fire during sleep)
    network_suspend_wdt();
    motion_suspend_wdt();

    // Step 5: Flush pending logs
    LOG_PANIC();

    // Step 6: Return — the idle thread will now run → WFI → ~2-5 µA
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

    // Step 1: Power on the modem (modem hardware + library)
    network_power_on();

    // Step 2: Resume task WDT entries
    network_resume_wdt();
    motion_resume_wdt();

    // Step 3: Restart heartbeat
    heartbeat_start();

    LOG_INF("Wake init complete");
}
```

---

### Phase 3: Motion Module Updates

#### [x] 3.1 GPIO SENSE investigation — **RESOLVED, no action needed**

**Finding:** `GPIO_INT_LEVEL_HIGH` on the nRF9160 does NOT use a GPIOTE
channel. The Zephyr nRF GPIO driver (gpio_nrfx.c) checks the trigger mode:

- **Edge** (`GPIO_INT_MODE_EDGE`): allocates a GPIOTE IN channel
- **Level** (`GPIO_INT_MODE_LEVEL`): uses only **SENSE** in PIN_CNF + **PORT event**

Since `configure_interrupt()` already calls `gpio_pin_interrupt_configure_dt()
with `GPIO_INT_LEVEL_HIGH`, the motion interrupt is already configured in the
most power-efficient way for System ON sleep. No switching between modes is
needed — the same configuration works for both active callback-driven operation
and low-power wake from WFI.

**No suspend/resume GPIO re-configuration is required.** The sensor stays in
its normal operating mode the whole time; only the nRF9160 side detects INT1.

#### [ ] 3.2 Task WDT suspend/resume

Store the `task_wdt_id` (currently a local) as a module-level variable,
then add suspend/resume WDT functions (see Phase 6).

---

### Phase 4: Network Module Updates

#### [ ] 4.1 Add suspend/resume functions

**File:** `src/modules/network/network.h`

```c
void network_suspend_wdt(void);
void network_resume_wdt(void);
```

Same WDT pattern as motion module.

#### [ ] 4.2 Modem power-on on wake

`wake_init()` calls `network_power_on()`, which calls
`lte_lc_power_on()` to power up the modem hardware and re-initialize
the modem library. After this, the modem is ready for
`lte_lc_connect_async()` to register and connect.

The personality then sends `NETWORK_CONNECT` as normal.
`state_disconnected_searching_entry()` calls
`lte_lc_connect_async(lte_lc_evt_handler)`, which re-registers
the event handler and starts the LTE attach sequence.

This avoids any sequencing issues — `wake_init()` handles the
modem power-on cleanly before the network thread processes
the connect request.

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

#### [x] 8.1 GPIOTE vs SENSE — **RESOLVED via driver source analysis**

The nRF GPIO Zephyr driver (gpio_nrfx.c) reveals:

```c
// gpio_nrfx_pin_interrupt_configure logic:
if (!edge_sense && mode == EDGE && dir == INPUT) {
    // Allocate a GPIOTE IN channel
    nrfx_gpiote_channel_get(...);
} else {
    // No GPIOTE channel — use SENSE + PORT event
    nrfx_gpiote_channel_free(...);  // free any previous channel
    trigger_config.p_in_channel = NULL;
}
```

Since `GPIO_INT_LEVEL_HIGH` is level mode (not edge), the `else` branch is
taken: **no GPIOTE channel is allocated**. The `pin_trigger_enable()` function
calls `nrfy_gpio_cfg_sense_set(pin, SENSE_HIGH)` and enables only the PORT
interrupt (`NRF_GPIOTE_INT_PORT_MASK`).

**Conclusion:** The motion interrupt is already using the GPIO PORT event
(DETECT signal) with SENSE. No GPIOTE channel is involved, so HFCLK is not
kept running by the motion interrupt. No fallback or reconfiguration is needed.

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
| `src/modules/motion/motion.h`   | Add WDT suspend/resume functions only (no GPIO re-config needed)                            |
| `src/modules/motion/motion.c`   | Store `task_wdt_id` at module scope, add WDT suspend/resume (no GPIO re-config needed)      |
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
- [x] GPIOTE does not prevent deep idle — driver analysis confirms SENSE+PORT event, no GPIOTE channel
- [ ] PWM0 does not keep HFCLK active when idle

---

## 7. Open Questions

1. **Does GPIOTE with an armed interrupt force HFCLK to stay on?**
   - **RESOLVED:** `GPIO_INT_LEVEL_HIGH` does NOT allocate a GPIOTE channel.
     It uses SENSE + PORT event, no HFCLK needed. See Phase 8.1.
   - Still worth measuring with a DMM to confirm no other source keeps HFCLK on.

2. **`k_timeout_t` max value for 7-day timer?**
   - `K_SECONDS(604800)` may overflow internal tick math (~18 h limit)
   - May need chained timers or RTC HAL compare registers

3. **`lte_lc_power_off()` + `lte_lc_power_on()` + re-connect sequencing?**
   - Does the network module handle full power cycle gracefully?
   - Need to test: power_off → wake → power_on → connect → works?

4. **Does the LIS2DTW12 SPI bus properly suspend via `CONFIG_PM_DEVICE_RUNTIME`?**
   - After sensor goes to power-down and no more SPI transactions,
     the SPI peripheral should clock-gate automatically
   - Verify with current measurement

5. **Race condition: motion fires during shutdown sequence?**
   - `enter_wfi_sleep()` disables motion interrupt first, then proceeds
   - If motion fires between entering sleeping_entry and disabling the
     interrupt, the message is published but no one handles it
   - Mitigation: disable interrupt first, then proceed with other steps
