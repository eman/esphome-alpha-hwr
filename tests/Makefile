# Makefile for GENI Protocol Unit Tests
# Compiles and runs host-based tests without ESP32 dependencies

CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -I. -I./mocks -I../
LDFLAGS =

# Header prerequisites. Without these, editing a component or mock header left
# every binary stale ("make: Nothing to be done") and the suite silently tested
# the previous build. Coarse on purpose: a header change rebuilds the suite,
# which takes seconds and is always correct.
COMPONENT_HDRS = $(wildcard ../components/alpha_hwr/*.h) \
                 $(wildcard ../components/dhw_demand/*.h)
MOCK_HDRS = $(shell find mocks -name '*.h' 2>/dev/null)
DEPS = $(COMPONENT_HDRS) $(MOCK_HDRS)

# Test executables
TESTS = test_protocol test_frame_parser test_read_chain_lifetime test_control_service test_control_state test_dhw_demand_logic test_dhw_demand_component test_transport_matching test_transport_fsm test_schedule_service test_write_operations test_schedule_codec test_telemetry_units test_pump_schedule_ux test_publish_gate test_dhw_publish_gate test_link_watchdog test_auth test_sensor_publisher test_telemetry_service test_device_info_service test_initial_read_retry test_subscribe_outcome test_failure_hold

# Default target
all: $(TESTS)

# Build test executables
PROTOCOL_SRCS = ../components/alpha_hwr/codec.cpp ../components/alpha_hwr/frame_builder.cpp

test_protocol: test_protocol.cpp protocol.h $(PROTOCOL_SRCS) $(DEPS)
	$(CXX) $(CXXFLAGS) test_protocol.cpp $(PROTOCOL_SRCS) -o test_protocol $(LDFLAGS)

test_frame_parser: test_frame_parser.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/codec.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) test_frame_parser.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/codec.cpp -o test_frame_parser $(LDFLAGS)

CONTROL_SRCS = ../components/alpha_hwr/control_service.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

test_control_service: test_control_service.cpp $(CONTROL_SRCS) $(DEPS)
	$(CXX) $(CXXFLAGS) test_control_service.cpp $(CONTROL_SRCS) -o test_control_service $(LDFLAGS)

test_control_state: test_control_state.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) test_control_state.cpp -o test_control_state $(LDFLAGS)

test_pump_schedule_ux: test_pump_schedule_ux.cpp ../components/alpha_hwr/pump_schedule_ux.h $(DEPS)
	$(CXX) $(CXXFLAGS) test_pump_schedule_ux.cpp -o test_pump_schedule_ux $(LDFLAGS)

test_publish_gate: test_publish_gate.cpp ../components/alpha_hwr/publish_gate.h $(DEPS)
	$(CXX) $(CXXFLAGS) test_publish_gate.cpp -o test_publish_gate $(LDFLAGS)

test_link_watchdog: test_link_watchdog.cpp ../components/alpha_hwr/link_watchdog.h $(DEPS)
	$(CXX) $(CXXFLAGS) test_link_watchdog.cpp -o test_link_watchdog $(LDFLAGS)

test_initial_read_retry: test_initial_read_retry.cpp ../components/alpha_hwr/initial_read_retry.h $(DEPS)
	$(CXX) $(CXXFLAGS) test_initial_read_retry.cpp -o test_initial_read_retry $(LDFLAGS)

# -Werror=switch on this target only. The exhaustiveness assertion inside can
# catch an enumerator added without a table entry, but NOT one added without a
# to_string() case -- that is a -Wswitch warning, and the suite does not build
# with -Werror, so it scrolls past. The two together make the claim true.
test_subscribe_outcome: test_subscribe_outcome.cpp ../components/alpha_hwr/subscribe_outcome.h $(DEPS)
	$(CXX) $(CXXFLAGS) -Werror=switch test_subscribe_outcome.cpp -o test_subscribe_outcome $(LDFLAGS)

# -Werror=switch here for the same reason, and it carries more weight: both
# release switches in failure_hold.h are switches over FailureHold, so a new
# hold added without deciding what releases it fails the build rather than
# becoming a permanent hold that silences every later fault.
test_failure_hold: test_failure_hold.cpp ../components/alpha_hwr/failure_hold.h $(DEPS)
	$(CXX) $(CXXFLAGS) -Werror=switch test_failure_hold.cpp -o test_failure_hold $(LDFLAGS)

test_dhw_publish_gate: test_dhw_publish_gate.cpp ../components/dhw_demand/publish_gate.h $(DEPS)
	$(CXX) $(CXXFLAGS) test_dhw_publish_gate.cpp -o test_dhw_publish_gate $(LDFLAGS)

test_dhw_demand_logic: test_dhw_demand_logic.cpp ../components/dhw_demand/dhw_demand_logic.h $(DEPS)
	$(CXX) $(CXXFLAGS) test_dhw_demand_logic.cpp -o test_dhw_demand_logic $(LDFLAGS)

# The component shell itself. Until this target existed, `esphome compile` was
# the only thing in the toolchain that compiled dhw_demand.cpp at all -- the
# unit suite, cppcheck and the mutation check all read past it, which is how a
# block referencing an out-of-scope variable once got through 905 green tests.
test_dhw_demand_component: test_dhw_demand_component.cpp ../components/dhw_demand/dhw_demand.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) test_dhw_demand_component.cpp ../components/dhw_demand/dhw_demand.cpp -o test_dhw_demand_component $(LDFLAGS)

test_transport_matching: test_transport_matching.cpp ../components/alpha_hwr/response_match.h $(DEPS)
	$(CXX) $(CXXFLAGS) test_transport_matching.cpp -o test_transport_matching $(LDFLAGS)

test_transport_fsm: test_transport_fsm.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) test_transport_fsm.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp -o test_transport_fsm $(LDFLAGS)

test_schedule_service: test_schedule_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) test_schedule_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp -o test_schedule_service $(LDFLAGS)

READ_CHAIN_SRCS = ../components/alpha_hwr/history_service.cpp ../components/alpha_hwr/event_log_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

test_read_chain_lifetime: test_read_chain_lifetime.cpp $(READ_CHAIN_SRCS) $(DEPS)
	$(CXX) $(CXXFLAGS) test_read_chain_lifetime.cpp $(READ_CHAIN_SRCS) -o test_read_chain_lifetime $(LDFLAGS)

WRITE_OP_SRCS = ../components/alpha_hwr/write_operation_service.cpp ../components/alpha_hwr/time_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/control_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

test_schedule_codec: test_schedule_codec.cpp ../components/alpha_hwr/schedule_codec.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) test_schedule_codec.cpp ../components/alpha_hwr/schedule_codec.cpp -o test_schedule_codec $(LDFLAGS)

test_telemetry_units: test_telemetry_units.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/codec.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) test_telemetry_units.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/codec.cpp -o test_telemetry_units $(LDFLAGS)

# -DUSE_TIME is not optional here. TimeService::current_time() -- the function
# that decides whether the pump is written at all, and that carries the SNTP
# floor -- lives inside that ifdef; without the define this target compiles the
# three-line #else stub and reports the real one as covered when it was never
# built. SET_CLOCK's confirm calls it on every readback, so the whole comparison
# would be untested too.
test_write_operations: test_write_operations.cpp $(WRITE_OP_SRCS) $(DEPS)
	$(CXX) $(CXXFLAGS) -DUSE_TIME test_write_operations.cpp $(WRITE_OP_SRCS) -o test_write_operations $(LDFLAGS)

AUTH_SRCS = ../components/alpha_hwr/auth.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

test_auth: test_auth.cpp $(AUTH_SRCS) $(DEPS)
	$(CXX) $(CXXFLAGS) test_auth.cpp $(AUTH_SRCS) -o test_auth $(LDFLAGS)

# -DUSE_TEXT_SENSOR is not optional here. The issue #127 publish-on-change
# guards for the alarm and warning text live inside that ifdef, and the
# component's AUTO_LOAD pulls text_sensor into every real build -- so without
# the define this target would compile a different program from the shipped one
# and report the guards as covered when they had not been compiled at all.
SENSOR_PUB_SRCS = ../components/alpha_hwr/sensor_publisher.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/codec.cpp

test_sensor_publisher: test_sensor_publisher.cpp $(SENSOR_PUB_SRCS) $(DEPS)
	$(CXX) $(CXXFLAGS) -DUSE_TEXT_SENSOR test_sensor_publisher.cpp $(SENSOR_PUB_SRCS) -o test_sensor_publisher $(LDFLAGS)

TELEMETRY_SVC_SRCS = ../components/alpha_hwr/telemetry_service.cpp ../components/alpha_hwr/sensor_publisher.cpp ../components/alpha_hwr/control_service.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

test_telemetry_service: test_telemetry_service.cpp $(TELEMETRY_SVC_SRCS) $(DEPS)
	$(CXX) $(CXXFLAGS) -DUSE_TEXT_SENSOR test_telemetry_service.cpp $(TELEMETRY_SVC_SRCS) -o test_telemetry_service $(LDFLAGS)

DEVICE_INFO_SRCS = ../components/alpha_hwr/device_info_service.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

test_device_info_service: test_device_info_service.cpp $(DEVICE_INFO_SRCS) $(DEPS)
	$(CXX) $(CXXFLAGS) test_device_info_service.cpp $(DEVICE_INFO_SRCS) -o test_device_info_service $(LDFLAGS)


# Run tests
test: $(TESTS)
	@echo "=========================================="
	@echo "Running Protocol Tests..."
	@echo "=========================================="
	@./test_protocol
	@echo ""
	@echo "=========================================="
	@echo "Running Frame Parser Tests..."
	@echo "=========================================="
	@./test_frame_parser
	@echo ""
	@echo "=========================================="
	@echo "Running Read Chain Lifetime Tests..."
	@echo "=========================================="
	@./test_read_chain_lifetime
	@echo ""
	@echo "=========================================="
	@echo "Running Control State Tests..."
	@echo "=========================================="
	@./test_control_state
	@echo ""
	@echo "=========================================="
	@echo "Running ControlService State Tests..."
	@echo "=========================================="
	@./test_control_service
	@echo ""
	@echo "=========================================="
	@echo "Running Pump/Schedule Reconciliation Tests..."
	@echo "=========================================="
	@./test_pump_schedule_ux
	@echo ""
	@echo "=========================================="
	@echo "Running DHW Detector Logic Tests..."
	@echo "=========================================="
	@./test_dhw_demand_logic
	@echo ""
	@echo "=========================================="
	@echo "Running DHW Detector Component Tests..."
	@echo "=========================================="
	@./test_dhw_demand_component
	@echo ""
	@echo "=========================================="
	@echo "Running Link Watchdog Tests..."
	@echo "=========================================="
	@./test_link_watchdog
	@./test_initial_read_retry
	@./test_subscribe_outcome
	@./test_failure_hold
	@echo ""
	@echo "=========================================="
	@echo "Running Transport Matching Tests..."
	@echo "=========================================="
	@./test_transport_matching
	@echo ""
	@echo "=========================================="
	@echo "Running Transport FSM Tests..."
	@echo "=========================================="
	@./test_transport_fsm
	@echo ""
	@echo "=========================================="
	@echo "Running Schedule Service Tests..."
	@echo "=========================================="
	@./test_schedule_service
	@echo ""
	@echo "=========================================="
	@echo "Running Write Operation Tests..."
	@echo "=========================================="
	@./test_write_operations
	@echo ""
	@echo "=========================================="
	@echo "Running Schedule Codec Tests..."
	@echo "=========================================="
	@./test_schedule_codec
	@echo ""
	@echo "=========================================="
	@echo "Running Telemetry Units Tests..."
	@echo "=========================================="
	@./test_telemetry_units
	@echo ""
	@echo "=========================================="
	@echo "Running Publish Gate Tests..."
	@echo "=========================================="
	@./test_publish_gate
	@echo ""
	@echo "=========================================="
	@echo "Running DHW Publish Gate Tests..."
	@echo "=========================================="
	@./test_dhw_publish_gate
	@echo ""
	@echo "=========================================="
	@echo "Running Authentication Handshake Tests..."
	@echo "=========================================="
	@./test_auth
	@echo ""
	@echo "=========================================="
	@echo "Running Sensor Publisher Tests..."
	@echo "=========================================="
	@./test_sensor_publisher
	@echo ""
	@echo "=========================================="
	@echo "Running Telemetry Service Tests..."
	@echo "=========================================="
	@./test_telemetry_service
	@echo ""
	@echo "=========================================="
	@echo "Running Device Info Service Tests..."
	@echo "=========================================="
	@./test_device_info_service

# Sanitizer run. On Linux, -fsanitize=address brings LeakSanitizer with it,
# which is what catches the read-chain closure leaks; LSan does not run on
# macOS ARM, so locally the weak_ptr assertions in test_read_chain_lifetime are
# the net instead. UBSan is set to abort rather than log-and-continue so a
# violation actually fails the build.
ASAN_CXXFLAGS = -std=c++17 -Wall -Wextra -O1 -g -I. -I./mocks -I../ \
                -fsanitize=address,undefined -fno-sanitize-recover=undefined \
                -fno-omit-frame-pointer
test-asan:
	$(MAKE) clean
	$(MAKE) test CXXFLAGS="$(ASAN_CXXFLAGS)" LDFLAGS="-fsanitize=address,undefined"

# Alternative targets for specific compilers
test-gcc: CXX = g++
test-gcc: clean test

test-clang: CXX = clang++
test-clang: clean test

# The Lovelace card. Plain node against a stubbed DOM -- no npm, no lockfile,
# nothing to install. Kept out of `test` so the mutation check (which drives
# `make test`) keeps running only what a C++ mutation can reach; `test-all`
# runs both. Absent node is a skip, not a failure: a C++-only checkout should
# still get a green suite.
test-js:
	@if command -v node >/dev/null 2>&1; then \
	  node js/test_schedule_card.js; \
	else \
	  echo "node not found; skipping the Lovelace card tests"; \
	fi

test-all: test test-js

# Clean build artifacts
clean:
	rm -f $(TESTS) *.o

# Help target
help:
	@echo "GENI Protocol Unit Test Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all         - Build all test executables (default)"
	@echo "  test        - Build and run all tests"
	@echo "  test-asan   - Build and run under ASan/UBSan (+LSan on Linux)"
	@echo "  test-gcc    - Build and run tests with GCC"
	@echo "  test-clang  - Build and run tests with Clang"
	@echo "  test-js     - Run the Lovelace card tests (node, no deps)"
	@echo "  test-all    - Run the C++ suite and the card tests"
	@echo "  clean       - Remove build artifacts"
	@echo "  help        - Show this help message"
	@echo ""
	@echo "Environment Variables:"
	@echo "  CXX         - C++ compiler (default: g++)"
	@echo "  CXXFLAGS    - Compiler flags"
	@echo "  LDFLAGS     - Linker flags"

.PHONY: all test test-asan test-gcc test-clang test-js test-all clean help
