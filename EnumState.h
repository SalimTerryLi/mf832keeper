//
// Created by salimterryli on 2020/12/27.
//

#ifndef ENUMSTATE_H
#define ENUMSTATE_H

#define STATE_NAME_LEN 32

enum STATE {
	UNKNOWN = 0,
	ERROR,
	TEST_AT,
	SETUP_PDP,
	SETUP_MS_MODE,
	PENDING_SETUP_RNDIS,
	SETUP_RNDIS,
	CONNECTED,
	__STATE_END // padded ending for compile check
};

// state_name[state_count][name_max_len]
extern const char state_name[][STATE_NAME_LEN];
// state_poll_timeout_s[state_count]
extern const int state_poll_timeout_s[];

const char *get_state_name(STATE state);

int get_state_poll_timeout_ms(STATE state);

bool should_perform_action(STATE state);


#endif //ENUMSTATE_H
