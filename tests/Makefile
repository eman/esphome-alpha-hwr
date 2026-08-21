# Makefile for GENI Protocol Unit Tests
# Compiles and runs host-based tests without ESP32 dependencies

CXX ?= g++
# Separate from CXXFLAGS so a caller can drop the optimiser without restating
# the whole flag line. tools/mutation_check.sh builds at -O0: it runs the suite
# hundreds of times and never reads a profile, so -O2 there is pure wall-clock.
OPT ?= -O2
CXXFLAGS = -std=c++17 -Wall -Wextra $(OPT) -I. -I./mocks -I../
LDFLAGS =

# ---------------------------------------------------------------------------
# Object cache
# ---------------------------------------------------------------------------
#
# Every target used to be one compiler invocation over its whole source list,
# so a full build compiled 142 translation units out of ~40 distinct files --
# codec.cpp fourteen times, transport.cpp ten. Header prerequisites were a
# single coarse $(DEPS) wildcard, so touching any header rebuilt all 29 targets
# from scratch.
#
# That was survivable until tests/test_component_wiring.cpp and
# tests/test_api_bridge.cpp arrived (issues #212 and #218). Each compiles 21
# translation units and both include alpha_hwr.h, so most entries in
# tools/mutation_check.sh now select at least one of them -- and a full mutation
# sweep went from minutes to the better part of an hour.
#
# Compiling once per translation unit and linking from objects fixes both: a
# mutation recompiles only the units whose include graph actually contains the
# mutated file, and -MMD records that graph exactly rather than guessing at it
# with a wildcard.
DEPFLAGS = -MMD -MP

# Objects live under a directory keyed by the compiler and flags that produced
# them. make has no idea flags changed, so without this a `make OPT=-O0` would
# happily link objects built at -O2, and `test-asan` -- which replaces CXXFLAGS
# wholesale -- would reuse un-instrumented objects and report a clean run
# against code the sanitizer never saw. Keying by flags also means the two
# variants coexist in the cache instead of evicting each other.
VARIANT := $(shell printf '%s' "$(CXX) $(CXXFLAGS)" | cksum | cut -d' ' -f1)
OBJDIR := .obj/$(VARIANT)

# The defines are NOT interchangeable, which is why objects are grouped by them
# rather than pooled. AGENTS.md §4 is explicit: -DUSE_TIME and -DUSE_TEXT_SENSOR
# change which code exists, so a target built without one compiles a different
# program and reports guards as covered that were never built. Sharing an object
# across those groups would reintroduce exactly that. -Werror=switch changes no
# code generation, but it gets its own group anyway rather than being applied
# everywhere, since turning a warning fatal across files that were never audited
# for it is a separate decision.
FLAGS_base :=
FLAGS_time := -DUSE_TIME
FLAGS_text := -DUSE_TEXT_SENSOR
FLAGS_sw   := -Werror=switch
GROUPS := base time text sw

# $(call objs,<group>,<sources>) -> object paths in that group's cache.
# ../components/x/y.cpp -> $(OBJDIR)/<group>/components/x/y.o
# test_z.cpp            -> $(OBJDIR)/<group>/test_z.o
objs = $(foreach s,$(2),$(OBJDIR)/$(1)/$(patsubst ../%,%,$(s:.cpp=.o)))

define GROUP_RULES
$(OBJDIR)/$(1)/components/%.o: ../components/%.cpp
	@mkdir -p $$(@D)
	$$(CXX) $$(CXXFLAGS) $$(FLAGS_$(1)) $$(DEPFLAGS) -c $$< -o $$@

$(OBJDIR)/$(1)/%.o: %.cpp
	@mkdir -p $$(@D)
	$$(CXX) $$(CXXFLAGS) $$(FLAGS_$(1)) $$(DEPFLAGS) -c $$< -o $$@
endef
$(foreach g,$(GROUPS),$(eval $(call GROUP_RULES,$(g))))

# CXXFLAGS on the link line too: test-asan puts -fsanitize in LDFLAGS, but a
# variant that ever carries it only in CXXFLAGS would link without the runtime.
LINK = $(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Header dependencies as recorded by the compiler on the last build. Absent on a
# cold cache, which is correct -- there is no object to be stale then either.
-include $(shell find $(OBJDIR) -name '*.d' 2>/dev/null)

# Test executables
TESTS = test_protocol test_frame_parser test_read_chain_lifetime test_control_service test_control_state test_dhw_demand_logic test_dhw_demand_component test_transport_matching test_transport_fsm test_schedule_service test_write_operations test_schedule_codec test_telemetry_units test_pump_schedule_ux test_publish_gate test_dhw_publish_gate test_link_watchdog test_readiness_watchdog test_sensor_publisher test_telemetry_service test_device_info_service test_initial_read_retry test_subscribe_outcome test_failure_hold test_pairing_stall test_gap_security_policy test_clock_sync_gate test_session test_component_wiring test_api_bridge test_time_service test_time_service_no_time test_limiter

# Objects are keyed by flags, but the BINARIES are not -- test_protocol is one
# path whichever variant produced it. So switching variants leaves executables
# on disk that were linked from a different object cache, and make cannot tell:
# their mtime is newer than the objects it would now use, so it reports nothing
# to be done and the suite runs the previous variant's binaries. Left alone
# that means `make test-asan` followed by `make test` silently re-runs the
# sanitized build, and `make OPT=-O0` after an -O2 build runs the -O2 one.
#
# Recording the variant and dropping the binaries when it changes costs one
# stat per invocation and removes the whole class. The objects stay, which is
# the point: switching back and forth is a relink, not a rebuild.
# Skipped under -n and -q. A dry run must not touch build state, and
# tools/mutation_targets.py depends on that: it reads the compile commands with
# `make -B -n` and its docstring promises the call "does not invalidate anyone's
# binaries". Without this guard, `make -n` deleted all 29 executables.
DRY_RUN := $(findstring n,$(firstword -$(MAKEFLAGS)))$(findstring q,$(firstword -$(MAKEFLAGS)))
LAST_VARIANT := $(shell cat .obj/last-variant 2>/dev/null)
ifeq ($(DRY_RUN),)
ifneq ($(LAST_VARIANT),$(VARIANT))
$(shell rm -f $(TESTS))
$(shell mkdir -p .obj && printf '%s' '$(VARIANT)' > .obj/last-variant)
endif
endif

# Default target
all: $(TESTS)

# ---------------------------------------------------------------------------
# Source sets
# ---------------------------------------------------------------------------
PROTOCOL_SRCS = ../components/alpha_hwr/codec.cpp ../components/alpha_hwr/frame_builder.cpp

FRAME_PARSER_SRCS = ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/codec.cpp

CONTROL_SRCS = ../components/alpha_hwr/control_service.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

TRANSPORT_FSM_SRCS = ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

TIME_SVC_SRCS = ../components/alpha_hwr/time_service.cpp $(TRANSPORT_FSM_SRCS)

SCHEDULE_SVC_SRCS = ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

READ_CHAIN_SRCS = ../components/alpha_hwr/history_service.cpp ../components/alpha_hwr/event_log_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

WRITE_OP_SRCS = ../components/alpha_hwr/write_operation_service.cpp ../components/alpha_hwr/time_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/control_service.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

# The whole component, shared by the two targets that drive it end to end.
COMPONENT_SRCS = $(WRITE_OP_SRCS) ../components/alpha_hwr/telemetry_service.cpp ../components/alpha_hwr/sensor_publisher.cpp ../components/alpha_hwr/device_info_service.cpp ../components/alpha_hwr/event_log_service.cpp ../components/alpha_hwr/history_service.cpp ../components/alpha_hwr/api_bridge.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/ble_connection_manager.cpp ../components/alpha_hwr/alpha_hwr.cpp


SENSOR_PUB_SRCS = ../components/alpha_hwr/sensor_publisher.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/codec.cpp

TELEMETRY_SVC_SRCS = ../components/alpha_hwr/telemetry_service.cpp ../components/alpha_hwr/sensor_publisher.cpp ../components/alpha_hwr/control_service.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/schedule_service.cpp ../components/alpha_hwr/schedule_codec.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

DEVICE_INFO_SRCS = ../components/alpha_hwr/device_info_service.cpp ../components/alpha_hwr/transport.cpp ../components/alpha_hwr/session.cpp ../components/alpha_hwr/frame_builder.cpp ../components/alpha_hwr/frame_parser.cpp ../components/alpha_hwr/codec.cpp

# ---------------------------------------------------------------------------
# Targets built with no extra defines
# ---------------------------------------------------------------------------
test_protocol: $(call objs,base,test_protocol.cpp $(PROTOCOL_SRCS))
	$(LINK)

test_frame_parser: $(call objs,base,test_frame_parser.cpp $(FRAME_PARSER_SRCS))
	$(LINK)

test_control_service: $(call objs,base,test_control_service.cpp $(CONTROL_SRCS))
	$(LINK)

test_control_state: $(call objs,base,test_control_state.cpp)
	$(LINK)

test_pump_schedule_ux: $(call objs,base,test_pump_schedule_ux.cpp)
	$(LINK)

test_publish_gate: $(call objs,base,test_publish_gate.cpp)
	$(LINK)

test_link_watchdog: $(call objs,base,test_link_watchdog.cpp)
	$(LINK)

test_readiness_watchdog: $(call objs,base,test_readiness_watchdog.cpp)
	$(LINK)

test_initial_read_retry: $(call objs,base,test_initial_read_retry.cpp)
	$(LINK)

test_pairing_stall: $(call objs,base,test_pairing_stall.cpp)
	$(LINK)

test_dhw_publish_gate: $(call objs,base,test_dhw_publish_gate.cpp)
	$(LINK)

test_dhw_demand_logic: $(call objs,base,test_dhw_demand_logic.cpp)
	$(LINK)

# The component shell itself. Until this target existed, `esphome compile` was
# the only thing in the toolchain that compiled dhw_demand.cpp at all -- the
# unit suite, cppcheck and the mutation check all read past it, which is how a
# block referencing an out-of-scope variable once got through 905 green tests.
test_dhw_demand_component: $(call objs,base,test_dhw_demand_component.cpp ../components/dhw_demand/dhw_demand.cpp)
	$(LINK)

test_session: $(call objs,base,test_session.cpp ../components/alpha_hwr/session.cpp)
	$(LINK)

test_transport_matching: $(call objs,base,test_transport_matching.cpp)
	$(LINK)

test_transport_fsm: $(call objs,base,test_transport_fsm.cpp $(TRANSPORT_FSM_SRCS))
	$(LINK)

test_schedule_service: $(call objs,base,test_schedule_service.cpp $(SCHEDULE_SVC_SRCS))
	$(LINK)

test_read_chain_lifetime: $(call objs,base,test_read_chain_lifetime.cpp $(READ_CHAIN_SRCS))
	$(LINK)

test_schedule_codec: $(call objs,base,test_schedule_codec.cpp ../components/alpha_hwr/schedule_codec.cpp)
	$(LINK)

test_telemetry_units: $(call objs,base,test_telemetry_units.cpp ../components/alpha_hwr/telemetry_decoder.cpp ../components/alpha_hwr/codec.cpp)
	$(LINK)

test_device_info_service: $(call objs,base,test_device_info_service.cpp $(DEVICE_INFO_SRCS))
	$(LINK)

# ---------------------------------------------------------------------------
# Targets built with -Werror=switch
# ---------------------------------------------------------------------------
# The exhaustiveness assertion inside can catch an enumerator added without a
# table entry, but NOT one added without a to_string() case -- that is a
# -Wswitch warning, and the suite does not build with -Werror, so it scrolls
# past. The two together make the claim true.
test_subscribe_outcome: $(call objs,sw,test_subscribe_outcome.cpp)
	$(LINK)

# Same reason, and it carries more weight: both release switches in
# failure_hold.h are switches over FailureHold, so a new hold added without
# deciding what releases it fails the build rather than becoming a permanent
# hold that silences every later fault.
test_failure_hold: $(call objs,sw,test_failure_hold.cpp)
	$(LINK)

# ble_connection_manager.cpp switches over GapSecurityAction at both reply
# sites, so a fourth action added without deciding what each handler does with
# it must break the build rather than silently take a default. The test mirrors
# that switch so the guard applies here too, where it is actually compiled.
test_gap_security_policy: $(call objs,sw,test_gap_security_policy.cpp)
	$(LINK)

# check_and_sync_time() switches over ClockSyncAction with no default, so a
# state added without deciding what that caller does with it is a compiler
# diagnostic there too -- but ESPHome's build does not use -Werror, so this
# target is where it is actually fatal. An earlier version of this comment
# claimed the production caller was covered while it used a ternary, which
# mapped every non-WARN_NO_TIME_ID state onto the wrong user-facing message.
test_clock_sync_gate: $(call objs,sw,test_clock_sync_gate.cpp)
	$(LINK)


# The same source as test_time_service, built WITHOUT -DUSE_TIME. That branch is
# otherwise compiled by no target at all, and replacing its body with a libc
# read does fail the tests -- so it is worth building.
#
# It does NOT verify the caller-side half of issue #270's "every caller refuses
# under `#ifndef USE_TIME`": no caller is linked into it. That half holds by
# construction (every caller goes through TimeService, and no
# `::time(nullptr)` remains in components/alpha_hwr/), which is a weaker
# guarantee than a test and is stated as such rather than implied.
test_time_service_no_time: $(call objs,base,test_time_service.cpp $(TIME_SVC_SRCS))
	$(LINK)

# limiter.h is a header plus codec.cpp for the float decode. Kept linkable on
# its own so the decode can be driven by real captured frames without a
# transport in the way.
test_limiter: $(call objs,base,test_limiter.cpp $(PROTOCOL_SRCS))
	$(LINK)

# ---------------------------------------------------------------------------
# Targets built with -DUSE_TIME
# ---------------------------------------------------------------------------
# Not optional here. TimeService::current_time() -- the function that decides
# whether the pump is written at all, and that carries the SNTP floor -- lives
# inside that ifdef; without the define these targets compile the three-line
# #else stub and report the real one as covered when it was never built.
# SET_CLOCK's confirm calls it on every readback, so the whole comparison would
# be untested too.
test_write_operations: $(call objs,time,test_write_operations.cpp $(WRITE_OP_SRCS))
	$(LINK)

test_component_wiring: $(call objs,time,test_component_wiring.cpp $(COMPONENT_SRCS))
	$(LINK)

test_time_service: $(call objs,time,test_time_service.cpp $(TIME_SVC_SRCS))
	$(LINK)

# The Home Assistant surface. Same source set as test_component_wiring because
# the bridge is reached through the component, and api_bridge.cpp needs the
# whole write-operation layer behind it to settle anything. Sharing that set is
# now literal: both link the same objects out of the cache.
test_api_bridge: $(call objs,time,test_api_bridge.cpp $(COMPONENT_SRCS))
	$(LINK)

# ---------------------------------------------------------------------------
# Targets built with -DUSE_TEXT_SENSOR
# ---------------------------------------------------------------------------
# Not optional here. The issue #127 publish-on-change guards for the alarm and
# warning text live inside that ifdef, and the component's AUTO_LOAD pulls
# text_sensor into every real build -- so without the define these targets would
# compile a different program from the shipped one and report the guards as
# covered when they had not been compiled at all.
test_sensor_publisher: $(call objs,text,test_sensor_publisher.cpp $(SENSOR_PUB_SRCS))
	$(LINK)

test_telemetry_service: $(call objs,text,test_telemetry_service.cpp $(TELEMETRY_SVC_SRCS))
	$(LINK)

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
	@./test_readiness_watchdog
	@./test_initial_read_retry
	@./test_subscribe_outcome
	@./test_failure_hold
	@./test_pairing_stall
	@./test_gap_security_policy
	@./test_clock_sync_gate
	@./test_time_service
	@./test_time_service_no_time
	@./test_limiter
	@./test_session
	@./test_component_wiring
	@./test_api_bridge
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
#
# Replacing CXXFLAGS wholesale changes VARIANT, so these objects land in their
# own cache directory and cannot be confused with the ordinary ones.
ASAN_CXXFLAGS = -std=c++17 -Wall -Wextra -O1 -g -I. -I./mocks -I../ \
                -fsanitize=address,undefined -fno-sanitize-recover=undefined \
                -fno-omit-frame-pointer
test-asan:
	$(MAKE) test CXXFLAGS="$(ASAN_CXXFLAGS)" LDFLAGS="-fsanitize=address,undefined"

# Alternative targets for specific compilers.
#
# Recursing with a command-line variable rather than a target-specific one, and
# that is the whole point: VARIANT is `:=`, evaluated once at parse time in
# global scope, so a target-specific `CXX = clang++` never reaches it. It only
# reached the recipes -- which meant `make test-clang` linked into the DEFAULT
# compiler's object cache, and once these targets lost their `clean` it compiled
# nothing at all and just re-ran the binaries g++ had built. A command-line
# variable is global in the sub-make, so VARIANT differs and each compiler keeps
# its own cache.
# The ILP32 leg: build the bridge's parser where `long` is 32 bits, which is
# the ESP32-C3's word size and the one this suite otherwise cannot express.
#
# Issue #255 is why. An epoch bound of 4294967295 was passed as a `long`
# parameter; on the device it narrowed to -1 and `set_single_event` and
# `set_vacation` refused every input a client could send. This suite listed
# `0,4294967295` among the ACCEPTED inputs the whole time and stayed green,
# because `long` is 64 bits on every host anyone runs it on. The bug was not
# uncovered -- it was covered by an assertion that could not fail here.
#
# -m32 removes that asymmetry rather than working around it: the SAME
# assertions, compiled at the target's word size. No new test cases, and no
# test that is honest only about a replica. Against the code that shipped #255,
# the existing `0,4294967295` case fails here.
#
# One binary, not the suite. test_api_bridge is where the argument parsing
# lives, and the point is the parser's width, not a second copy of everything.
#
# x86 Linux with gcc-multilib only -- there is no -m32 on arm64 macOS. Absent
# support is a skip with a reason rather than a failure, on the same principle
# as the Lovelace target below: a checkout that cannot build it is not broken.
# CI checks for the toolchain separately, so a missing multilib there fails the
# job instead of quietly skipping it.
ILP32_CXXFLAGS = -std=c++17 -Wall -Wextra $(OPT) -m32 -I. -I./mocks -I../
test-ilp32:
	@printf 'int main(){return 0;}' > .ilp32-probe.c; \
	if $(CXX) -m32 .ilp32-probe.c -o .ilp32-probe >/dev/null 2>&1; then \
	  rm -f .ilp32-probe .ilp32-probe.c; \
	  echo "== test_api_bridge at ILP32 (long is 32 bits, as on the ESP32-C3) =="; \
	  $(MAKE) test_api_bridge CXXFLAGS="$(ILP32_CXXFLAGS)" LDFLAGS="-m32" && ./test_api_bridge; \
	else \
	  rm -f .ilp32-probe .ilp32-probe.c; \
	  echo "SKIP: $(CXX) cannot target -m32 here (needs x86 Linux + gcc-multilib)."; \
	  echo "      This leg is what catches a bound that narrows on the ESP32-C3 (issue #255)."; \
	fi

test-gcc:
	$(MAKE) test CXX=g++

test-clang:
	$(MAKE) test CXX=clang++

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

# Clean build artifacts, object cache included -- every variant of it, since the
# point of a clean is to leave nothing behind that a later build might reuse.
clean:
	rm -f $(TESTS) *.o *.d
	rm -rf .obj

# Binaries only, object cache kept. tools/mutation_check.sh ends with this plus
# a targeted purge of the objects it could have mutated: a full clean there
# would throw away objects built from unmutated sources, which is most of the
# cache and most of the next run's speed.
clean-bin:
	rm -f $(TESTS)

# Where this invocation's objects live. tools/mutation_check.sh asks so it can
# clear the cache for the flags IT is about to use without touching the other
# variants -- a plain `clean` would take a developer's -O2 objects with it.
print-objdir:
	@echo $(OBJDIR)

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
	@echo "  clean       - Remove build artifacts and the object cache"
	@echo "  help        - Show this help message"
	@echo ""
	@echo "Environment Variables:"
	@echo "  CXX         - C++ compiler (default: g++)"
	@echo "  OPT         - Optimisation level (default: -O2; mutation runs use -O0)"
	@echo "  CXXFLAGS    - Compiler flags"
	@echo "  LDFLAGS     - Linker flags"
	@echo ""
	@echo "Objects are cached under .obj/<flags-hash>/<group>/, so switching"
	@echo "OPT, CXX or sanitizers keeps both variants rather than rebuilding."

.PHONY: all test test-asan test-ilp32 test-gcc test-clang test-js test-all clean clean-bin print-objdir help
