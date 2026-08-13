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
TESTS = test_protocol test_frame_parser test_read_chain_lifetime test_control_service test_control_state test_dhw_demand_logic test_transport_matching test_transport_fsm test_schedule_service test_write_operations test_schedule_codec test_telemetry_units test_pump_schedule_ux test_publish_gate test_dhw_publish_gate

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

test_dhw_publish_gate: test_dhw_publish_gate.cpp ../components/dhw_demand/publish_gate.h $(DEPS)
	$(CXX) $(CXXFLAGS) test_dhw_publish_gate.cpp -o test_dhw_publish_gate $(LDFLAGS)

test_dhw_demand_logic: test_dhw_demand_logic.cpp ../components/dhw_demand/dhw_demand_logic.h $(DEPS)
	$(CXX) $(CXXFLAGS) test_dhw_demand_logic.cpp -o test_dhw_demand_logic $(LDFLAGS)

test_transport_matching: test_transport_matching.cpp ../components/alpha_hwr/response_match.h $(DEPS)
	$(CXX) $(CXXFLAGS) test_transport_matching.cpp -o test_transport_matching $(LDFLAGS)

test_transport_fsm: test_transport_fsm.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) test_transport_fsm.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp -o test_transport_fsm $(LDFLAGS)

test_schedule_service: test_schedule_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) test_schedule_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp -o test_schedule_service $(LDFLAGS)

READ_CHAIN_SRCS = ../components/alpha_hwr/history_service.cpp ../components/alpha_hwr/event_log_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

test_read_chain_lifetime: test_read_chain_lifetime.cpp $(READ_CHAIN_SRCS) $(DEPS)
	$(CXX) $(CXXFLAGS) test_read_chain_lifetime.cpp $(READ_CHAIN_SRCS) -o test_read_chain_lifetime $(LDFLAGS)

WRITE_OP_SRCS = ../components/alpha_hwr/write_operation_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/control_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

test_schedule_codec: test_schedule_codec.cpp ../components/alpha_hwr/schedule_codec.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) test_schedule_codec.cpp ../components/alpha_hwr/schedule_codec.cpp -o test_schedule_codec $(LDFLAGS)

test_telemetry_units: test_telemetry_units.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/codec.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) test_telemetry_units.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/codec.cpp -o test_telemetry_units $(LDFLAGS)

test_write_operations: test_write_operations.cpp $(WRITE_OP_SRCS) $(DEPS)
	$(CXX) $(CXXFLAGS) test_write_operations.cpp $(WRITE_OP_SRCS) -o test_write_operations $(LDFLAGS)


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
	@echo "  clean       - Remove build artifacts"
	@echo "  help        - Show this help message"
	@echo ""
	@echo "Environment Variables:"
	@echo "  CXX         - C++ compiler (default: g++)"
	@echo "  CXXFLAGS    - Compiler flags"
	@echo "  LDFLAGS     - Linker flags"

.PHONY: all test test-asan test-gcc test-clang clean help
