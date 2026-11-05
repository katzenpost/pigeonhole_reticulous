# The screens on the desktop: kq-sim draws KqApp as the T-Deck would, into PNG screenshots, with
# the chat client behind it talking to a replica through relay.py.
#   make LVGL_DIR=<lvgl v9.6 checkout> [SPANGAP_LCD=<spangap-lcd straddle>]
#   python3 relay.py --config <reticulum config dir> --socket kq.sock &
#   (or, with no Reticulum, the host tests' in-memory replica: make build/fake-relay; build/fake-relay kq.sock &)
#   build/kq-sim --socket kq.sock --fonts $(SPANGAP_LCD)/esp-idf/fonts --state <dir> [--name <name>]
include ../kq.mk

SPANGAP_LCD ?= $(ROOT)/../../spangap-lcd
ifndef LVGL_DIR
$(error LVGL_DIR names an LVGL v9.6 checkout: git clone -b v9.6.0 https://github.com/lvgl/lvgl)
endif

LVGL_C := $(shell find $(LVGL_DIR)/src -name '*.c')
LVGL_O := $(patsubst $(LVGL_DIR)/src/%.c,$(B)/lvgl/%.o,$(LVGL_C))
LV_FLAGS := -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL_DIR) -I$(LVGL_DIR)/include/lvgl $(shell pkg-config --cflags freetype2)
APP := $(ROOT)/conditional/spangap-lcd
VOICE := $(ROOT)/conditional/lxst
SIM_FLAGS := $(CXXFLAGS) $(LV_FLAGS) -Ishim -I../host -I$(APP)/include -I$(VOICE)/include -I$(SPANGAP_LCD)/esp-idf/include -Wno-missing-field-initializers -Wno-deprecated-declarations
SIM_O := $(B)/sim.o $(B)/sim_client.o $(B)/kq_lcd.o $(B)/kq_radio_app.o $(B)/lcd_input_box.o

$(B)/kq-sim: $(SIM_O) $(QR_O) $(FSKV_O) $(B)/fskv_room.o $(KQ_O) $(VOICE_O) $(SODIUM_O) $(CTIDH_O) $(LVGL_O)
	$(CXX) $^ -o $@ $(LDLIBS) $(shell pkg-config --libs freetype2) -lpthread

$(B)/fake-relay: fake_relay.cpp ../host/fake_replica.h $(KQ_O) $(SODIUM_O) $(CTIDH_O)
	$(CXX) $(CXXFLAGS) -I../host $< $(KQ_O) $(SODIUM_O) $(CTIDH_O) -o $@ $(LDLIBS)

$(B)/lvgl/%.o: $(LVGL_DIR)/src/%.c lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g $(LV_FLAGS) -w -c $< -o $@

$(B)/%.o: %.cpp sim.h lv_conf.h $(wildcard shim/*.h) $(wildcard $(ROOT)/include/*.h)
	@mkdir -p $(B)
	$(CXX) $(SIM_FLAGS) -c $< -o $@

$(B)/kq_lcd.o: $(APP)/src/kq_lcd.cpp $(APP)/include/kq_app.h $(ROOT)/include/kq_view.h $(QR)/kq_qr.h
	@mkdir -p $(B)
	$(CXX) $(SIM_FLAGS) -c $< -o $@

$(B)/kq_radio_app.o: $(VOICE)/src/kq_radio_app.cpp $(VOICE)/include/kq_radio_app.h $(ROOT)/include/kq_view.h
	@mkdir -p $(B)
	$(CXX) $(SIM_FLAGS) -c $< -o $@

$(B)/fskv_room.o: ../host/fskv_room.c ../host/fskv_room.h
	@mkdir -p $(B)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -D_DEFAULT_SOURCE -c $< -o $@

# Copied first, so its own directory's lcd_internal.h does not shadow the shim's.
$(B)/lcd_input_box.cpp: $(SPANGAP_LCD)/esp-idf/src/lcd_ui/lcd_input_box.cpp
	@mkdir -p $(B)
	cp $< $@
$(B)/lcd_input_box.o: $(B)/lcd_input_box.cpp
	$(CXX) $(SIM_FLAGS) -c $< -o $@

clean:
	rm -rf build
.PHONY: clean
