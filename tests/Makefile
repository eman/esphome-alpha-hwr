# Makefile for GENI Protocol Unit Tests
# Compiles and runs host-based tests without ESP32 dependencies

CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -I. -I./mocks -I../
LDFLAGS =

# Test executables
TESTS = test_protocol test_control_state test_dhw_demand_logic test_transport_matching test_transport_fsm test_schedule_service test_write_operations test_schedule_codec test_telemetry_units test_pump_schedule_ux

# Default target
all: $(TESTS)

# Build test executables
test_protocol: test_protocol.cpp protocol.h
	$(CXX) $(CXXFLAGS) test_protocol.cpp -o test_protocol $(LDFLAGS)

test_control_state: test_control_state.cpp
	$(CXX) $(CXXFLAGS) test_control_state.cpp -o test_control_state $(LDFLAGS)

test_pump_schedule_ux: test_pump_schedule_ux.cpp ../components/alpha_hwr/pump_schedule_ux.h
	$(CXX) $(CXXFLAGS) test_pump_schedule_ux.cpp -o test_pump_schedule_ux $(LDFLAGS)

test_dhw_demand_logic: test_dhw_demand_logic.cpp ../components/dhw_demand/dhw_demand_votes.h
	$(CXX) $(CXXFLAGS) test_dhw_demand_logic.cpp -o test_dhw_demand_logic $(LDFLAGS)

test_transport_matching: test_transport_matching.cpp
	$(CXX) $(CXXFLAGS) test_transport_matching.cpp -o test_transport_matching $(LDFLAGS)

test_transport_fsm: test_transport_fsm.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp
	$(CXX) $(CXXFLAGS) test_transport_fsm.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp -o test_transport_fsm $(LDFLAGS)

test_schedule_service: test_schedule_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp
	$(CXX) $(CXXFLAGS) test_schedule_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp -o test_schedule_service $(LDFLAGS)

WRITE_OP_SRCS = ../components/alpha_hwr/write_operation_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/control_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

test_schedule_codec: test_schedule_codec.cpp ../components/alpha_hwr/schedule_codec.cpp
	$(CXX) $(CXXFLAGS) test_schedule_codec.cpp ../components/alpha_hwr/schedule_codec.cpp -o test_schedule_codec $(LDFLAGS)

test_telemetry_units: test_telemetry_units.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/codec.cpp
	$(CXX) $(CXXFLAGS) test_telemetry_units.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/codec.cpp -o test_telemetry_units $(LDFLAGS)

test_write_operations: test_write_operations.cpp $(WRITE_OP_SRCS)
	$(CXX) $(CXXFLAGS) test_write_operations.cpp $(WRITE_OP_SRCS) -o test_write_operations $(LDFLAGS)


# Run tests
test: $(TESTS)
	@echo "=========================================="
	@echo "Running Protocol Tests..."
	@echo "=========================================="
	@./test_protocol
	@echo ""
	@echo "=========================================="
	@echo "Running Control State Tests..."
	@echo "=========================================="
	@./test_control_state
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
	@echo "  test-gcc    - Build and run tests with GCC"
	@echo "  test-clang  - Build and run tests with Clang"
	@echo "  clean       - Remove build artifacts"
	@echo "  help        - Show this help message"
	@echo ""
	@echo "Environment Variables:"
	@echo "  CXX         - C++ compiler (default: g++)"
	@echo "  CXXFLAGS    - Compiler flags"
	@echo "  LDFLAGS     - Linker flags"

.PHONY: all test test-gcc test-clang clean help
