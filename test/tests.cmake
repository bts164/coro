# Every test source file, listed once, with the builds it belongs to. The keywords
# are described in cmake/coro_test.cmake; in short:
#
#   DESKTOP            desktop executable, linked with the desktop coro library
#   PICO_STUB          in test_pico_suite: Pico configuration, stubbed SDK, on the host
#   HOST_LINK <lib>... host executable linked with these libraries instead (named
#                      <file>_pico if the file is DESKTOP as well)
#   ON_TARGET <image>  in the firmware image coro_target_<image>, run on the board
#
# To add a test file, add one line. To run a file on another platform, add the
# keyword to its line. To leave single tests out on a platform, guard them in the
# source with the preprocessor (#ifndef CORO_PICO today).
#
# On-target images: each holds about 325 tests at most on an RP2040, limited by RAM
# (doc/design/on_target_tests.md). Start another image rather than fill one up.

# detail/
coro_test(detail/test_poll_result.cpp            DESKTOP PICO_STUB ON_TARGET core)
coro_test(detail/test_waker_context.cpp          DESKTOP PICO_STUB ON_TARGET core)
coro_test(detail/test_intrusive_list.cpp         DESKTOP PICO_STUB ON_TARGET core)
coro_test(detail/test_rc.cpp                     DESKTOP PICO_STUB ON_TARGET core)
coro_test(detail/test_frame_pool.cpp             DESKTOP PICO_STUB ON_TARGET core)
coro_test(detail/test_poller.cpp                 DESKTOP)
coro_test(detail/test_timer_queue.cpp            DESKTOP PICO_STUB ON_TARGET core)

# Foundational types: coro.h, future.h, stream.h, co_invoke.h
coro_test(test_future.cpp                        DESKTOP PICO_STUB ON_TARGET core)
coro_test(test_future_ref.cpp                    DESKTOP PICO_STUB ON_TARGET core)
coro_test(test_stream.cpp                        DESKTOP PICO_STUB ON_TARGET core)
coro_test(test_coro.cpp                          DESKTOP PICO_STUB ON_TARGET core)
coro_test(test_coro_stream.cpp                   DESKTOP PICO_STUB ON_TARGET core)
coro_test(test_co_invoke.cpp                     DESKTOP PICO_STUB ON_TARGET core)

# runtime/
coro_test(runtime/test_runtime.cpp               DESKTOP PICO_STUB ON_TARGET runtime)
coro_test(runtime/test_runtime_shutdown.cpp      DESKTOP)
coro_test(runtime/test_executor_task.cpp         DESKTOP PICO_STUB ON_TARGET runtime)
coro_test(runtime/test_current_thread_executor.cpp DESKTOP)
coro_test(runtime/test_work_sharing_executor.cpp DESKTOP)
coro_test(runtime/test_work_stealing_executor.cpp DESKTOP)
coro_test(runtime/test_work_stealing_io.cpp      DESKTOP)
coro_test(runtime/test_spawn_on.cpp              DESKTOP)
coro_test(runtime/test_local_run_queue.cpp       DESKTOP)
coro_test(runtime/test_io_driver.cpp             DESKTOP)

# task/
coro_test(task/test_join_handle.cpp              DESKTOP PICO_STUB ON_TARGET runtime)
coro_test(task/test_join_set.cpp                 DESKTOP PICO_STUB ON_TARGET core)
coro_test(task/test_stream_handle.cpp            DESKTOP PICO_STUB ON_TARGET core)
coro_test(task/test_coro_scope.cpp               DESKTOP PICO_STUB ON_TARGET runtime)
coro_test(task/test_spawn_blocking.cpp           DESKTOP)
# Fibers: on the host these run on the desktop backend. On the board it is the real
# ARM context switch on a real stack. Not in the stub suite, which has no fibers.
coro_test(task/test_fiber.cpp                    DESKTOP ON_TARGET core)
# alloc_fiber_stack()/free_fiber_stack() of the Pico backend are plain C++, so they
# run on the host as they are.
coro_test(task/test_fiber_context_pico_alloc.cpp HOST_LINK coro_pico_core)
# The Pico context-switch assembly under an ARM emulator. CMakeLists.txt adds the
# cross-assembled binary it loads.
coro_test(task/test_switch_context_pico.cpp      HOST_LINK unicorn::unicorn)

# sync/
coro_test(sync/test_event.cpp                    DESKTOP PICO_STUB ON_TARGET sync)
coro_test(sync/test_select.cpp                   DESKTOP PICO_STUB ON_TARGET sync)
coro_test(sync/test_when.cpp                     DESKTOP PICO_STUB ON_TARGET sync)
coro_test(sync/test_sleep.cpp                    DESKTOP PICO_STUB ON_TARGET sync)
coro_test(sync/test_oneshot.cpp                  DESKTOP PICO_STUB ON_TARGET sync)
coro_test(sync/test_mpsc.cpp                     DESKTOP PICO_STUB ON_TARGET sync)
coro_test(sync/test_watch.cpp                    DESKTOP PICO_STUB ON_TARGET sync)
coro_test(sync/test_broadcast.cpp                DESKTOP PICO_STUB ON_TARGET sync)
# Pico only. IsrTrigger (isr_trigger.h) makes the interrupt: a hardware timer alarm
# on the board, a thread on the host.
coro_test(sync/test_isr_event.cpp                PICO_STUB ON_TARGET sync)

# io/ — TCP and UDP are one file each for every backend: the desktop sockets, lwIP
# built for the host (test_tcp_stream_pico, test_udp_socket_pico), and lwIP on the
# board, where they use the loopback interface and no radio.
coro_test(io/test_file.cpp                       DESKTOP)
coro_test(io/test_lookup_host.cpp                DESKTOP)
coro_test(io/test_pipe.cpp                       DESKTOP)
# Not on the board: circular_byte_buffer.cpp is not part of the Pico library.
coro_test(io/test_circular_byte_buffer.cpp       DESKTOP)
coro_test(io/test_signal.cpp                     DESKTOP)
coro_test(io/test_tcp_stream.cpp                 DESKTOP HOST_LINK coro_lwip_tcp ON_TARGET net)
coro_test(io/test_udp_socket.cpp                 DESKTOP HOST_LINK coro_lwip_udp ON_TARGET net)
coro_test(io/test_ws_stream.cpp                  DESKTOP)

# pico/ — Pico-only code, on the host: the HAL against stubbed hardware (a thread
# stands in for the interrupt), the MQTT client against lwIP built for the host.
# Not on the board: these tests drive the stubs (set a pin level, complete a DMA
# channel, raise the IRQ by hand) and a fake broker, so the board needs tests of its
# own against the real peripherals, and an image that links coro_pico_hal.
coro_test(pico/test_async_dma.cpp                HOST_LINK coro_pico_hal_core)
coro_test(pico/test_gpio.cpp                     HOST_LINK coro_pico_hal_core)
coro_test(pico/test_mqtt_client.cpp              HOST_LINK coro_lwip_mqtt)

# stress/
coro_test(stress/test_skynet.cpp                 DESKTOP)
