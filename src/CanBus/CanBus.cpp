/*
 * PiCCANTE - PiCCANTE Car Controller Area Network Tool for Exploration
 * Copyright (C) 2025 Peter Repukat
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "CanBus.hpp"

#include <cstdint>
#include <cstddef>
#include <hardware/irq.h>
#include <hardware/platform_defs.h>
#include <hardware/regs/intctrl.h>
#include "can2040.h"
#include "projdefs.h"
#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"
#include "Logger/Logger.hpp"
#include <array>
#include "fmt.hpp"
#include <hardware/structs/pio.h>
#include <lfs.h>
#include <fs/littlefs_driver.hpp>
#include "led/led.hpp"
#include <hardware/watchdog.h>
#include <hardware/gpio.h>
#include <utility>
#include "mitm_bridge/bridge.hpp"
#include <hardware/pio.h>


namespace piccante::can {


struct CanQueues {
    QueueHandle_t rx;
    QueueHandle_t tx;
};

namespace {
// NOLINTNEXTLINE: cppcoreguidelines-avoid-non-const-global-variables
std::array<CanQueues, NUM_BUSSES> can_queues = {};
// NOLINTNEXTLINE: cppcoreguidelines-avoid-non-const-global-variables
std::array<can2040, NUM_BUSSES> can_buses = {};
constexpr std::array<uint8_t, 3> pwr_pins = {piccanteCAN0_PWR_PIN, piccanteCAN1_PWR_PIN,
                                             piccanteCAN2_PWR_PIN};

TaskHandle_t canTaskHandle = nullptr; // NOLINT

TaskHandle_t rx_task_handle = nullptr; // NOLINT

} // namespace

struct CanGPIO {
    uint8_t pin_rx;
    uint8_t pin_tx;
    uint8_t pio_num;
    uint8_t pio_irq;
};


constexpr std::array<const CanGPIO, NUM_BUSSES> CAN_GPIO = {
    {{
         .pin_rx = piccanteCAN0_RX_PIN,
         .pin_tx = piccanteCAN0_TX_PIN,
         .pio_num = 0,
         .pio_irq = PIO0_IRQ_0,
     },
#if piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_2 ||                                       \
    piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_3
     {
         .pin_rx = piccanteCAN1_RX_PIN,
         .pin_tx = piccanteCAN1_TX_PIN,
         .pio_num = 1,
         .pio_irq = PIO1_IRQ_0,
     },
#endif
#if piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_3
     {
         .pin_rx = piccanteCAN2_RX_PIN,
         .pin_tx = piccanteCAN2_TX_PIN,
         .pio_num = 2,
         .pio_irq = PIO2_IRQ_0,
     }
#endif
    }};


#pragma pack(push, 1)
struct can_settings_file {
    uint8_t num_busses;
    std::array<CanSettings, 3> bus_config;
    std::array<uint8_t, 2> bridged;
};
#pragma pack(pop)

namespace {


std::array<uint32_t, piccanteNUM_CAN_BUSSES> rx_overflow_counts = {0};
std::array<uint32_t, piccanteNUM_CAN_BUSSES> tx_overflow_counts = {0};

// NOLINTNEXTLINE: cppcoreguidelines-avoid-non-const-global-variables
can_settings_file settings = {};

void can2040_cb_can(struct can2040* cd, uint32_t notify, // NOLINT
                    struct can2040_msg* msg) {           // NOLINT
    BaseType_t higher_priority_task_woken = pdFALSE;
    // Add message processing code here...
    if (notify == CAN2040_NOTIFY_RX) {
        frame fr{
            .extended = msg->id & CAN2040_ID_EFF,
            .rtr = msg->id & CAN2040_ID_RTR,
            .id = msg->id & ~(CAN2040_ID_RTR | CAN2040_ID_EFF),
            .dlc = msg->dlc,
            .data = {msg->data[0], msg->data[1], msg->data[2], msg->data[3], msg->data[4],
                     msg->data[5], msg->data[6], msg->data[7]},
        };
        const auto bus_num = cd->pio_num; // NOLINT
        // Process received message - add to queue from ISR
        if (xQueueSendFromISR(can_queues[bus_num].rx, &fr, &higher_priority_task_woken) !=
            pdTRUE) {
            UBaseType_t uxSavedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
            rx_overflow_counts[bus_num]++;
            taskEXIT_CRITICAL_FROM_ISR(uxSavedInterruptStatus);
        }
        // if (rx_task_handle != nullptr) {
        //     xTaskNotifyFromISR(rx_task_handle, 0, eSetBits,
        //     &higher_priority_task_woken);
        // }
    }
    // else if (notify == CAN2040_NOTIFY_TX) {
    //     // Process transmitted message
    // } else if (notify == CAN2040_NOTIFY_ERROR) {
    //     // Handle error
    // }
    portYIELD_FROM_ISR(higher_priority_task_woken); // NOLINT
}

void PIOx_IRQHandler_CAN0() {
    BaseType_t const higher_priority_task_woken = pdFALSE;
    can2040_pio_irq_handler(&(can_buses[0]));
    portYIELD_FROM_ISR(higher_priority_task_woken); // NOLINT
}
#if piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_2 ||                                       \
    piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_3

void PIOx_IRQHandler_CAN1() {
    BaseType_t const higher_priority_task_woken = pdFALSE;
    can2040_pio_irq_handler(&(can_buses[1]));
    portYIELD_FROM_ISR(higher_priority_task_woken); // NOLINT
}
#endif
#if piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_3

void PIOx_IRQHandler_CAN2() {
    BaseType_t const higher_priority_task_woken = pdFALSE;
    can2040_pio_irq_handler(&(can_buses[2]));
    portYIELD_FROM_ISR(higher_priority_task_woken); // NOLINT
}
#endif


std::array<bool, 3> canbus_initial_setup_done = {
    false,
#if piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_2 ||                                       \
    piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_3
    false,
#endif
#if piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_3
    false,
#endif
};

void canbus_setup_initial(uint8_t bus) {
    for (auto pin : pwr_pins) {
        if (pin == 0) {
            continue;
        }
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }

#ifdef WIFI_ENABLED
    PIO pio_instance = pio_get_instance(CAN_GPIO[bus].pio_num);
    if (CAN_GPIO[bus].pio_num == 2) {
        uint sm_mask = 0b0011;
        pio_claim_sm_mask(pio_instance, sm_mask);
        Log::info << "Claimed PIO2 state machines 0-2 for CAN bus " << bus
                  << ", leaving SM3 for CYW43\n";
    } else {
        uint sm_mask = 0x0F;
        pio_claim_sm_mask(pio_instance, sm_mask);
        Log::info << "Claimed all PIO" << CAN_GPIO[bus].pio_num
                  << " state machines for CAN bus " << bus << "\n";
    }
#endif

    can2040_setup(&(can_buses[bus]), CAN_GPIO[bus].pio_num);


    switch (bus) { // NOLINT
        case 0:
            can2040_callback_config(&(can_buses[bus]), can2040_cb_can);
            irq_set_exclusive_handler(CAN_GPIO[bus].pio_irq, PIOx_IRQHandler_CAN0);
            break;
#if piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_2 ||                                       \
    piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_3
        case 1:
            can2040_callback_config(&(can_buses[bus]), can2040_cb_can);
            irq_set_exclusive_handler(CAN_GPIO[bus].pio_irq, PIOx_IRQHandler_CAN1);
            break;
#endif
#if piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_3

        case 2:
            can2040_callback_config(&(can_buses[bus]), can2040_cb_can);
            irq_set_exclusive_handler(CAN_GPIO[bus].pio_irq, PIOx_IRQHandler_CAN2);
            break;
#endif
        default:
            Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
            return;
    }

    irq_set_enabled(CAN_GPIO[bus].pio_irq, false);
    // Set core affinity for this interrupt

    if (CAN_GPIO[bus].pio_num == 0) {
        hw_set_bits(&pio0_hw->inte1, (1u << CAN_GPIO[bus].pio_irq));
        hw_clear_bits(&pio0_hw->inte0,
                      (1u << CAN_GPIO[bus].pio_irq)); // Disable on core 0
    } else if (CAN_GPIO[bus].pio_num == 1) {
        hw_set_bits(&pio1_hw->inte1, (1u << CAN_GPIO[bus].pio_irq));
        hw_clear_bits(&pio1_hw->inte0,
                      (1u << CAN_GPIO[bus].pio_irq)); // Disable on core 0
    }
#if piccanteNUM_CAN_BUSSES == piccanteCAN_NUM_3
    else if (CAN_GPIO[bus].pio_num == 2) {
        hw_set_bits(&pio2_hw->inte1, (1u << CAN_GPIO[bus].pio_irq));
        hw_clear_bits(&pio2_hw->inte0,
                      (1u << CAN_GPIO[bus].pio_irq)); // Disable on core 0
    }
#endif

    irq_set_priority(CAN_GPIO[bus].pio_irq, configMAX_SYSCALL_INTERRUPT_PRIORITY + 1);
    irq_set_enabled(CAN_GPIO[bus].pio_irq, true);
    canbus_initial_setup_done[bus] = true;
}

void canbus_setup(uint8_t bus, uint32_t bitrate) {
    if (!canbus_initial_setup_done[bus]) {
        canbus_setup_initial(bus);
    }

    can2040_start(&(can_buses[bus]), SYS_CLK_HZ, bitrate, CAN_GPIO[bus].pin_rx,
                  CAN_GPIO[bus].pin_tx);
}

SemaphoreHandle_t settings_mutex = nullptr;

void store_settings() {
    Log::debug << "Storing CAN settings...\n";
    if (settings_mutex == nullptr ||
        xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        Log::error << "Failed to take settings mutex for store_settings\n";
        return;
    }

    lfs_file_t writeFile;
    const int err = lfs_file_open(&piccante::fs::lfs, &writeFile, "can_settings",
                                  LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err == LFS_ERR_OK) {
        if (lfs_file_write(&piccante::fs::lfs, &writeFile, &settings, sizeof(settings)) <
            0) {
            Log::error << "Failed to write CAN settings file\n";
        }
        if (const auto err = lfs_file_close(&piccante::fs::lfs, &writeFile);
            err != LFS_ERR_OK) {
            Log::error << "Failed to close CAN settings file: " << fmt::sprintf("%d", err)
                       << "\n";
        }
    } else {
        Log::error << "Failed to write CAN settings file\n";
    }
    xSemaphoreGive(settings_mutex);
}

void canTask(void* parameters) {
    (void)parameters;

    Log::info << "Starting CAN task...\n";

    for (std::size_t i = 0; i < NUM_BUSSES; i++) {
        can_queues[i].rx = xQueueCreate(CAN_QUEUE_SIZE, sizeof(frame));
        can_queues[i].tx = xQueueCreate(CAN_QUEUE_SIZE, sizeof(frame));

        if (can_queues[i].rx == NULL || can_queues[i].tx == NULL) {
            Log::error << "Failed to create CAN queues for bus " << i << "\n";
            return;
        }
    }

    for (std::size_t i = 0; i < settings.num_busses && i < piccanteNUM_CAN_BUSSES; i++) {
        canbus_setup_initial(i);
        if (settings.bus_config[i].enabled) {
            Log::info << "Enabling CAN bus " << fmt::sprintf("%d", i) << " with bitrate "
                      << settings.bus_config[i].bitrate << " from stored settings\n";
            canbus_setup(i, settings.bus_config[i].bitrate);
        }
    }


    frame msg = {};
    for (;;) {
        bool did_tx = false;
        for (std::size_t i = 0; i < NUM_BUSSES; i++) {
            if (xQueueReceive(can_queues[i].tx, &msg, 0) == pdTRUE) {
                did_tx = true;
                if (!can2040_check_transmit(&(can_buses[i]))) {
                    can2040_pio_irq_handler(&(can_buses[i]));
                }

                can2040_msg msg2040 = {
                    .id = msg.id | (msg.extended ? CAN2040_ID_EFF : 0) |
                          (msg.rtr ? CAN2040_ID_RTR : 0),
                    .dlc = msg.dlc,
                    .data = {msg.data[0], msg.data[1], msg.data[2], msg.data[3],
                             msg.data[4], msg.data[5], msg.data[6], msg.data[7]}};
                int res = can2040_transmit(&(can_buses[i]), &msg2040);
                if (res < 0) {
                    Log::error << "CAN" << fmt::sprintf("%d", i)
                               << ": Failed to send message\n";
                }
            }
        }
        if (did_tx) {
            led::blink();
        } else {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        }
    }
}

} // namespace

TaskHandle_t& create_task() {
    xTaskCreate(canTask, "CAN", configMINIMAL_STACK_SIZE, nullptr, CAN_TASK_PRIORITY,
                &canTaskHandle);
    return canTaskHandle;
}

int send_can(uint8_t bus, const frame& msg) {
    if (bus >= piccanteNUM_CAN_BUSSES || bus >= settings.num_busses) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        return -1;
    }
    if (!settings.bus_config[bus].enabled) {
        Log::error << "CAN bus " << fmt::sprintf("%d", bus) << " is not enabled\n";
        return -1;
    }
    if (settings.bus_config[bus].listen_only) {
        Log::error << "CAN bus " << fmt::sprintf("%d", bus)
                   << " is in listen-only mode\n";
        return -1;
    }
    if (xQueueSend(can_queues[bus].tx, &msg, pdMS_TO_TICKS(CAN_QUEUE_TIMEOUT_MS)) !=
        pdTRUE) {
        Log::error << "CAN bus " << fmt::sprintf("%d", bus) << ": TX queue full\n";
        tx_overflow_counts[bus]++;
        return -1;
    }
    xTaskNotifyGive(canTaskHandle);
    return 0;
}
int receive(uint8_t bus, frame& msg, uint32_t timeout_ms) {
    if (bus >= piccanteNUM_CAN_BUSSES || bus >= settings.num_busses) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return -1;
    }
    if (!settings.bus_config[bus].enabled) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return -1;
    }
    if (xQueueReceive(can_queues[bus].rx, &msg, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return (uint8_t)uxQueueMessagesWaiting(can_queues[bus].rx);
    }
    return -1;
}

int get_can_rx_buffered_frames(uint8_t bus) {
    if (bus >= piccanteNUM_CAN_BUSSES) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        return -1;
    }
    return (uint8_t)uxQueueMessagesWaiting(can_queues[bus].rx);
}
int get_can_tx_buffered_frames(uint8_t bus) {
    if (bus >= piccanteNUM_CAN_BUSSES) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        return -1;
    }
    return (uint8_t)uxQueueMessagesWaiting(can_queues[bus].tx);
}


uint32_t get_can_rx_overflow_count(uint8_t bus) {
    if (bus >= piccanteNUM_CAN_BUSSES) {
        return 0;
    }
    return rx_overflow_counts[bus];
}
uint32_t get_can_tx_overflow_count(uint8_t bus) {
    if (bus >= piccanteNUM_CAN_BUSSES) {
        return 0;
    }
    return tx_overflow_counts[bus];
}

bool get_statistics(uint8_t bus, can2040_stats& stats) {
    if (bus >= piccanteNUM_CAN_BUSSES) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        return false;
    }

    // Access the can2040 instance for the specified bus
    can2040_get_statistics(&can_buses[bus], &stats);
    return true;
}

void set_num_busses(uint8_t num_busses) {
    if (num_busses > NUM_BUSSES) {
        Log::error << "Invalid number of CAN buses: " << fmt::sprintf("%d", num_busses)
                   << "\n";
        return;
    }
    settings.num_busses = num_busses;
    store_settings();
    // reset board.
    watchdog_enable(0, false);
    while (1) { /* Wait for watchdog to trigger */
    }
}
uint8_t get_num_busses() { return settings.num_busses; }

void enable(uint8_t bus, uint32_t bitrate) {
    if (bus >= piccanteNUM_CAN_BUSSES || bus >= settings.num_busses) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        return;
    }
    if (settings.bus_config[bus].enabled) {
        Log::warning << "CAN bus " << fmt::sprintf("%d", bus)
                     << " is already enabled - resetting\n";
        set_bitrate(bus, bitrate);
        return;
    }
    if (pwr_pins[bus] != 0) {
        gpio_put(pwr_pins[bus], true);
    }
    canbus_setup(bus, bitrate);
    settings.bus_config[bus].bitrate = bitrate;
    settings.bus_config[bus].enabled = true;
    store_settings();
}

void disable(uint8_t bus) {
    if (bus >= piccanteNUM_CAN_BUSSES || bus >= settings.num_busses) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        return;
    }
    can2040_stop(&(can_buses[bus]));
    if (pwr_pins[bus] != 0) {
        gpio_put(pwr_pins[bus], false);
    }
    if (!settings.bus_config[bus].enabled) {
        settings.bus_config[bus].enabled = false;
        store_settings();
    }
}

void set_bitrate(uint8_t bus, uint32_t bitrate) {
    if (bus >= piccanteNUM_CAN_BUSSES || bus >= settings.num_busses) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        return;
    }
    if (settings.bus_config[bus].enabled) {
        can2040_stop(&(can_buses[bus]));
        canbus_setup(bus, bitrate);
        settings.bus_config[bus].enabled = true;
    }
    if (settings.bus_config[bus].bitrate != bitrate) {
        settings.bus_config[bus].bitrate = bitrate;
        store_settings();
    }
}

bool is_enabled(uint8_t bus) {
    if (bus >= piccanteNUM_CAN_BUSSES || bus >= settings.num_busses) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        return false;
    }
    return settings.bus_config[bus].enabled;
}
uint32_t get_bitrate(uint8_t bus) {
    if (bus >= piccanteNUM_CAN_BUSSES || bus >= settings.num_busses) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        return DEFAULT_BUS_SPEED;
    }
    return settings.bus_config[bus].bitrate;
}

bool is_listenonly(uint8_t bus) {
    if (bus >= piccanteNUM_CAN_BUSSES || bus >= settings.num_busses) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        return false;
    }
    return settings.bus_config[bus].listen_only;
}

void set_listenonly(uint8_t bus, bool listen_only) {
    if (bus >= piccanteNUM_CAN_BUSSES || bus >= settings.num_busses) {
        Log::error << "Invalid CAN bus number: " << fmt::sprintf("%d", bus) << "\n";
        return;
    }
    if (settings.bus_config[bus].listen_only == listen_only) {
        return;
    }
    settings.bus_config[bus].listen_only = listen_only;
    store_settings();
}

void load_settings() {
    settings_mutex = xSemaphoreCreateMutex();
    if (settings_mutex == nullptr) {
        Log::error << "Failed to create settings mutex\n";
        return;
    }

    lfs_file_t readFile;
    const int err =
        lfs_file_open(&piccante::fs::lfs, &readFile, "can_settings", LFS_O_RDONLY);
    if (err == LFS_ERR_OK) {
        lfs_ssize_t const bytesRead =
            lfs_file_read(&piccante::fs::lfs, &readFile, &settings, sizeof(settings));
        lfs_file_close(&piccante::fs::lfs, &readFile);
    } else {
        Log::error << "Failed to read CAN settings file\n";
    }
    mitm::bridge::set_bridge(settings.bridged[0], settings.bridged[1]);
}

void set_rx_task_handle(TaskHandle_t task_handle) { rx_task_handle = task_handle; }

void store_bridge_settings(const std::pair<uint8_t, uint8_t>& bridge) {
    if (bridge.first == settings.bridged[0] && bridge.second == settings.bridged[1]) {
        return;
    }
    settings.bridged[0] = bridge.first;
    settings.bridged[1] = bridge.second;
    store_settings();
}


} // namespace piccante::can