//
// Created by salimterryli on 2020/12/27.
//

#include "EnumState.h"

// state_name[state_count][name_max_len]
const char state_name[][STATE_NAME_LEN]={
		{"UNKNOWN"},
		{"AUTO_TEST_AT"},
		{"AUTO_SETUP_PDP"},
		{"AUTO_SETUP_MS_MODE"},
		{"AUTO_WAIT_NETWORK"},
		{"AUTO_SETUP_RNDIS"},
		{"CONNECTED"}

};
// state_poll_timeout_s[state_count]
const int state_poll_timeout_s[]={
		-1,
		3,
		3,
		3,
		10,
		3,
		-1
};

static_assert(sizeof(state_name)/STATE_NAME_LEN == static_cast<int>(__STATE_END), "state_name Enum not match");
static_assert(sizeof(state_poll_timeout_s) / sizeof(int) == static_cast<int>(__STATE_END), "state_poll_timeout_s Enum not match");

const char* get_state_name(STATE state){
	return state_name[static_cast<int>(state)];
}

int get_state_poll_timeout_ms(STATE state){
	return state_poll_timeout_s[static_cast<int>(state)] * 1000;
}