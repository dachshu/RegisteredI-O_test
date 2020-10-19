#pragma once
#include "common.h"
#include "Zone.h"
#include "protocol.h"
#include <set>

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };


struct OVER_EX {
	WSAOVERLAPPED over;
	WSABUF	wsabuf[1];

	char* net_buf;
	EVENT_TYPE	event_type;
	int idx;
};


struct SOCKETINFO
{
	OVER_EX* recv_over;
	//char	pre_net_buf[MAX_BUFFER];
	int		prev_packet_size;
	int		recv_buf_start_idx;
	int		origin_offset;
	SOCKET	socket;
	int		idx;
	int		gid;
	char	name[MAX_STR_LEN];

	bool is_connected;
	bool is_active;
	short	x, y;
	unsigned	move_time;
	std::set <int> near_id;

	int my_woker_id;
	ZoneNodeBuffer zone_node_buffer;
	std::set<int> broadcast_zone;
	int my_zone_col;
	int my_zone_row;
};
