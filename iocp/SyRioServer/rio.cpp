#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <unordered_map>
#include <queue>
#include <algorithm>
#include <iterator>
#include "common.h"
#include "protocol.h"
#include "MessageQueue.h"
#include "Zone.h"
#include "MessageBuffer.h"

using namespace std;
using namespace chrono;



// max_client �� �޵��� ó���ʿ�
//atomic_int num_client{ 0 };




GlobalRecvBufferPool globalBufferPool;
LocalSendBufferPool sendBufferPool[NUM_WORKER_THREADS];
HANDLE	g_iocp[NUM_WORKER_THREADS];

const unsigned int zone_width = int(WORLD_WIDTH / ZONE_SIZE);
const unsigned int zone_heigt = int(WORLD_HEIGHT / ZONE_SIZE);
Zone zone[zone_heigt][zone_width];

const unsigned int per_max_clients = int(MAX_CLIENT / 2);
//thread_local int new_user_id = 0;
//thread_local unordered_map<int, SOCKETINFO*> my_clients;
thread_local SOCKETINFO* my_clients[per_max_clients];
thread_local int empty_cli_idx[per_max_clients];
thread_local int num_my_clients = 0;




void Disconnect(int id);

void error_display(const char* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	cout << msg;
	wcout << L"���� " << lpMsgBuf << endl;
	//while (true);
	LocalFree(lpMsgBuf);
}


bool is_near(int x1, int y1, int x2, int y2)
{
	if (VIEW_RANGE < abs(x1 - x2)) return false;
	if (VIEW_RANGE < abs(y1 - y2)) return false;
	return true;
}

void send_packet(SOCKET sock, OVER_EX* send_buf, int cid)
{
	int ret = WSASend(sock, send_buf->wsabuf, 1, 0, 0, &send_buf->over, 0);
	if (0 != ret) {
		int err_no = WSAGetLastError();
		if ((WSA_IO_PENDING != err_no) && (WSAECONNRESET != err_no)
			|| (WSAECONNABORTED != err_no) || (WSAENOTSOCK != err_no)) {
			//error_display("WSASend Error :", err_no);
			return;
		}
	}
}

void send_login_ok_packet(SOCKET sock, int gid, int x, int y, int cid)
{
	OVER_EX* send_over = sendBufferPool[tid].get_sendBuffer();
	memset(&(send_over->over), 0x00, sizeof(OVERLAPPED));

	sc_packet_login_ok* packet = reinterpret_cast<sc_packet_login_ok*>(send_over->net_buf);

	packet->id = gid;
	packet->size = sizeof(sc_packet_login_ok);
	packet->type = SC_LOGIN_OK;
	packet->x = x;
	packet->y = y;
	packet->hp = 100;
	packet->level = 1;
	packet->exp = 1;
	send_over->wsabuf[0].len = sizeof(sc_packet_login_ok);
	send_packet(sock, send_over, cid);
}


void send_login_fail(SOCKET sock, int gid, int cid)
{
	OVER_EX* send_over = sendBufferPool[tid].get_sendBuffer();
	memset(&(send_over->over), 0x00, sizeof(OVERLAPPED));

	sc_packet_login_fail* packet = reinterpret_cast<sc_packet_login_fail*>(send_over->net_buf);

	packet->size = sizeof(sc_packet_login_fail);
	packet->type = SC_LOGIN_FAIL;
	send_over->wsabuf[0].len = sizeof(sc_packet_login_fail);
	send_packet(sock, send_over, cid);
}

void send_put_object_packet(SOCKET sock, int new_gid, int nx, int ny, int cid)
{
	OVER_EX* send_over = sendBufferPool[tid].get_sendBuffer();
	memset(&(send_over->over), 0x00, sizeof(OVERLAPPED));
	sc_packet_put_object* packet = reinterpret_cast<sc_packet_put_object*>(send_over->net_buf);

	packet->id = new_gid;
	packet->size = sizeof(sc_packet_put_object);
	packet->type = SC_PUT_OBJECT;
	packet->x = nx;
	packet->y = ny;
	packet->o_type = 1;
	send_over->wsabuf[0].len = sizeof(sc_packet_put_object);
	send_packet(sock, send_over, cid);
}

void send_pos_packet(SOCKET sock, int mover, int mx, int my, unsigned mv_time, int cid)
{
	OVER_EX* send_over = sendBufferPool[tid].get_sendBuffer();
	memset(&(send_over->over), 0x00, sizeof(OVERLAPPED));
	sc_packet_pos* packet = reinterpret_cast<sc_packet_pos*>(send_over->net_buf);
	packet->id = mover;
	packet->size = sizeof(sc_packet_pos);
	packet->type = SC_POS;
	packet->x = mx;
	packet->y = my;
	packet->move_time = mv_time;
	send_over->wsabuf[0].len = sizeof(sc_packet_pos);

	send_packet(sock, send_over, cid);
}


void send_remove_object_packet(SOCKET sock, int leaver, int cid)
{
	OVER_EX* send_over = sendBufferPool[tid].get_sendBuffer();
	memset(&(send_over->over), 0x00, sizeof(OVERLAPPED));
	sc_packet_remove_object* packet = reinterpret_cast<sc_packet_remove_object*>(send_over->net_buf);
	packet->id = leaver;
	packet->size = sizeof(sc_packet_remove_object);
	packet->type = SC_REMOVE_OBJECT;
	send_over->wsabuf[0].len = sizeof(sc_packet_remove_object);
	send_packet(sock, send_over, cid);
}

void send_chat_packet(SOCKET sock, int teller, char* mess, int cid)
{
	OVER_EX* send_over = sendBufferPool[tid].get_sendBuffer();
	memset(&(send_over->over), 0x00, sizeof(OVERLAPPED));
	sc_packet_chat* packet = reinterpret_cast<sc_packet_chat*>(send_over->net_buf);
	packet->id = teller;
	packet->size = sizeof(sc_packet_chat);
	packet->type = SC_CHAT;
	send_over->wsabuf[0].len = sizeof(sc_packet_chat);
	send_packet(sock, send_over, cid);
}



void Disconnect(int id)
{
	if (my_clients[id] == nullptr) return;
	//std::cout << "client [" << id << "] closed" << std::endl;
	//num_client.fetch_add(-1, memory_order_release);
	
	//1. zone 에서 제거
	zone[my_clients[id]->my_zone_row][my_clients[id]->my_zone_col].Remove(tid, id, my_clients[id]->zone_node_buffer);

	//2. broadcast
	for (auto i : my_clients[id]->broadcast_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::BYE, -1, -1, 0, my_clients[id]->gid);
	}


	my_clients[id]->is_connected = false;
	closesocket(my_clients[id]->socket);
	globalBufferPool.return_recvBuffer(my_clients[id]->recv_over);

	delete my_clients[id];
	my_clients[id] = nullptr;;
	empty_cli_idx[per_max_clients - num_my_clients] = id;
	--num_my_clients;
	//my_clients.erase(id);
}

void get_new_zone(set<int>& new_zone, int x, int y) {
	int x1 = int((x - VIEW_RANGE) / ZONE_SIZE);
	if (x1 < 0) x1 = 0;
	int x2 = int((x + VIEW_RANGE) / ZONE_SIZE);
	if (x2 >= zone_width) x2 = zone_width - 1;
	int y1 = int((y - VIEW_RANGE) / ZONE_SIZE);
	if (y1 < 0) y1 = 0;
	int y2 = int((y + VIEW_RANGE) / ZONE_SIZE);
	if (y2 >= zone_heigt) y2 = zone_heigt - 1;

	new_zone.insert(x1 * zone_width + y1);
	new_zone.insert(x1 * zone_width + y2);
	new_zone.insert(x2 * zone_width + y1);
	new_zone.insert(x2 * zone_width + y2);
}


void ProcessMove(int id, unsigned char dir)
{
	short x = my_clients[id]->x;
	short y = my_clients[id]->y;
	
	switch (dir) {
	case D_UP: if (y > 0) y--;
		break;
	case D_DOWN: if (y < WORLD_HEIGHT - 1) y++;
		break;
	case D_LEFT: if (x > 0) x--;
		break;
	case D_RIGHT: if (x < WORLD_WIDTH - 1) x++;
		break;
	case 99:
		x = rand() % WORLD_WIDTH;
		y = rand() % WORLD_HEIGHT;
		break;
	default: cout << "Invalid Direction Error\n";
		//while (true);
		break;
	}

	my_clients[id]->x = x;
	my_clients[id]->y = y;

	send_pos_packet(my_clients[id]->socket, my_clients[id]->gid, x, y, my_clients[id]->move_time, id);

	// 1. zone in/out
	int zc = int(x / ZONE_SIZE);
	int zr = int(y / ZONE_SIZE);
	if (zc != my_clients[id]->my_zone_col || zr != my_clients[id]->my_zone_row) {
		ZoneNode* zn = my_clients[id]->zone_node_buffer.get();
		zn->cid = id; zn->worker_id = tid;
		zone[zr][zc].Add(zn);
		zone[my_clients[id]->my_zone_row][my_clients[id]->my_zone_col].Remove(tid, id, my_clients[id]->zone_node_buffer);
		my_clients[id]->my_zone_col = zc;
		my_clients[id]->my_zone_row = zr;
	}

	// 2. broadcast
	for (auto i : my_clients[id]->broadcast_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::MOVE, x, y, my_clients[id]->move_time, my_clients[id]->gid);
	}
	set<int> new_zone;
	get_new_zone(new_zone, x, y);

	// ���� ��� �� zone
	for (auto i : new_zone) {
		if (my_clients[id]->broadcast_zone.count(i) == 0) {
			zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::MOVE, x, y, my_clients[id]->move_time, my_clients[id]->gid);
		}
	}
	my_clients[id]->broadcast_zone.swap(new_zone);

}

void ProcessLogin(int user_id, char* id_str)
{
	strcpy_s(my_clients[user_id]->name, id_str);
	my_clients[user_id]->is_connected = true;
	send_login_ok_packet(my_clients[user_id]->socket, my_clients[user_id]->gid
		, my_clients[user_id]->x, my_clients[user_id]->y, user_id);
	send_put_object_packet(my_clients[user_id]->socket, my_clients[user_id]->gid
		, my_clients[user_id]->x, my_clients[user_id]->y, user_id);

	// 1. zone in
	int zc = int(my_clients[user_id]->x / ZONE_SIZE);
	int zr = int(my_clients[user_id]->y / ZONE_SIZE);
	//int idx = get_empty_zone_nodeIdx(my_clients[user_id]);
	int idx = -1;
	
	ZoneNode* zn = my_clients[user_id]->zone_node_buffer.get();
	zn->cid = user_id; zn->worker_id = tid;
	zone[zr][zc].Add(zn);
	my_clients[user_id]->my_zone_col = zc;
	my_clients[user_id]->my_zone_row = zr;
	

	// 2. broadcast
	set<int> new_zone;
	get_new_zone(new_zone, my_clients[user_id]->x, my_clients[user_id]->y);

	// ���� ��� �� zone
	for (auto i : new_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, user_id, Msg::MOVE
			, my_clients[user_id]->x, my_clients[user_id]->y, my_clients[user_id]->move_time, my_clients[user_id]->gid);
	}
	my_clients[user_id]->broadcast_zone.swap(new_zone);
}


bool ProcessPacket(int id, void* buff)
{
	char* packet = reinterpret_cast<char*>(buff);
	switch (packet[1]) {
	case CS_LOGIN: {
		cs_packet_login* login_packet = reinterpret_cast<cs_packet_login*>(packet);
		ProcessLogin(id, login_packet->id);
	}
				 break;
	case CS_MOVE: {
		cs_packet_move* move_packet = reinterpret_cast<cs_packet_move*>(packet);
		my_clients[id]->move_time = move_packet->move_time;
		ProcessMove(id, move_packet->direction);
	}
				break;
	case CS_ATTACK:
		break;
	case CS_CHAT: {
		cs_packet_chat* chat_packet = reinterpret_cast<cs_packet_chat*>(packet);
		//ProcessChat(id, chat_packet->chat_str);
	}
				break;
	case CS_LOGOUT:
		break;
	case CS_TELEPORT:
		ProcessMove(id, 99);
		break;
	default: 
		cout << "Invalid Packet Type Error\n";
		//while (true);
		return false;
		break;
	}
	return true;
}

void handle_recv(unsigned int uid, OVER_EX* ovex, ULONG bytes) {
	int recv_buf_start_idx = my_clients[uid]->recv_buf_start_idx;
	char* p = &(ovex->net_buf[recv_buf_start_idx]);
	int remain = bytes;
	int packet_size;
	int prev_packet_size = my_clients[uid]->prev_packet_size;

	if (0 == prev_packet_size)
		packet_size = 0;
	else packet_size = my_clients[uid]->recv_over->net_buf[recv_buf_start_idx - prev_packet_size];

	int rem = 0;
	while (remain > 0) {
		if (0 == packet_size) packet_size = p[0];
		int required = packet_size - prev_packet_size;
		if (required <= remain) {
			//memcpy(my_clients[uid]->pre_net_buf + prev_packet_size, p, required);
			ProcessPacket(uid, &(ovex->net_buf[recv_buf_start_idx - prev_packet_size]));
			remain -= required;
			p += required;
			recv_buf_start_idx += required;
			prev_packet_size = 0;
			packet_size = 0;
		}
		else {
			//memcpy(my_clients[uid]->pre_net_buf + prev_packet_size, p, remain);4
			memcpy(ovex->net_buf, &ovex->net_buf[recv_buf_start_idx - prev_packet_size], prev_packet_size);
			prev_packet_size += remain;
			recv_buf_start_idx = prev_packet_size;
			remain = 0;
			rem = 1;
		}
	}
	recv_buf_start_idx = rem * recv_buf_start_idx;

	my_clients[uid]->recv_buf_start_idx = recv_buf_start_idx;
	my_clients[uid]->prev_packet_size = prev_packet_size;

	ovex->wsabuf[0].buf = &my_clients[uid]->recv_over->net_buf[recv_buf_start_idx];
	ovex->wsabuf[0].len = MAX_BUFFER - recv_buf_start_idx;

	DWORD flags = 0;
	memset(&ovex->over, 0x00, sizeof(OVERLAPPED));
	int ret = WSARecv(my_clients[uid]->socket, ovex->wsabuf, 1, 0, &flags, &ovex->over, 0);
	if (0 != ret) {
		int err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no)
			error_display("WSARecv Error :", err_no);
	}
}

void handle_move_msg(MsgNode* msg) {
	int my_id = msg->to;
	int mx = my_clients[my_id]->x;
	int my = my_clients[my_id]->y;

	// �þ� �˻�
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. ó�� ���� ��
	if (in_view && (my_clients[my_id]->near_id.count(msg->gid) == 0)) {
		// list�� �ְ� ������ send_put_packet
		my_clients[my_id]->near_id.insert(msg->gid);
		send_put_object_packet(my_clients[my_id]->socket, msg->gid, msg->x, msg->y, my_id);
		// ������� HI �����ֱ�
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::HI, mx, my, my_clients[my_id]->move_time, msg->from_cid, my_clients[my_id]->gid);
		return;
	}

	// 2. �˴� ��
	if (in_view && (my_clients[my_id]->near_id.count(msg->gid) != 0)) {
		// �׳� ������ send_pos_packet
		send_pos_packet(my_clients[my_id]->socket, msg->gid, msg->x, msg->y
			, msg->move_time, my_id);
		return;
	}

	// 3. ������� ��
	if (!in_view && (my_clients[my_id]->near_id.count(msg->gid) != 0)) {
		// list���� ���� send_remove_packet
		my_clients[my_id]->near_id.erase(msg->gid);
		send_remove_object_packet(my_clients[my_id]->socket, msg->gid, my_id);
		// ������׵� BYE �����ֱ�
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::BYE, -1, -1, 0, msg->from_cid, my_clients[my_id]->gid);
		return;
	}
}

void handle_hi_msg(MsgNode* msg) {
	int my_id = msg->to;
	int mx = my_clients[my_id]->x;
	int my = my_clients[my_id]->y;
	// 시야 검사
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. 처음 보는 애
	if (in_view && (my_clients[my_id]->near_id.count(msg->gid) == 0)) {
		// list에 넣고 나한테 send_put_packet
		my_clients[my_id]->near_id.insert(msg->gid);
		send_put_object_packet(my_clients[my_id]->socket, msg->gid, msg->x, msg->y, my_id);
	}
}

void handle_bye_msg(MsgNode* msg) {
	int my_id = msg->to;
	int mx = my_clients[my_id]->x;
	int my = my_clients[my_id]->y;

	if (my_clients[my_id]->near_id.count(msg->gid) != 0) {
		my_clients[my_id]->near_id.erase(msg->gid);
		send_remove_object_packet(my_clients[my_id]->socket, msg->gid, my_id);
	}
}


void handle_new_client(MsgNode* msg) {
	int user_id = empty_cli_idx[per_max_clients - 1 - num_my_clients];
	++num_my_clients;

	SOCKETINFO* new_player = reinterpret_cast<SOCKETINFO*>(msg->info);
	new_player->idx = user_id;
	new_player->my_woker_id = tid;

	new_player->zone_node_buffer.set(new_player->my_woker_id, new_player->idx);

	my_clients[user_id] = new_player;
	//my_clients.insert(make_pair(user_id, new_player));
	HANDLE r = CreateIoCompletionPort(reinterpret_cast<HANDLE>(new_player->socket), g_iocp[tid], user_id, 0);

	DWORD flags = 0;
	memset(&(new_player->recv_over->over), 0x00, sizeof(OVERLAPPED));
	int ret = WSARecv(new_player->socket, new_player->recv_over->wsabuf
		, 1, 0, &flags, &new_player->recv_over->over, 0);
	if (0 != ret) {
		int err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no)
			error_display("WSARecv Error :", err_no);
	}

}


const int MAX_RIO_REUSLTS = int(MAX_CLIENT / NUM_WORKER_THREADS);
void do_worker(int t)
{
	tid = t;
	for (int i = 0; i < per_max_clients; ++i) {
		empty_cli_idx[i] = per_max_clients - 1 - i;
	}
	//high_resolution_clock::time_point last_checked_time = high_resolution_clock::now();
	//unsigned long long processed_io = 0;

	while (true) {
		//auto duration = high_resolution_clock::now() - last_checked_time;
		//if (1000 * 10 < duration_cast<milliseconds>(duration).count()) {
		//	cout << "thread[" << tid << "] : " << processed_io << endl;
		//	processed_io = 0;
		//	last_checked_time = high_resolution_clock::now();
		//}

		int checked_queue = 0;
		while (checked_queue++ < MAX_RIO_REUSLTS * 16)
		{
			MsgNode* msg = msgQueue[tid].Deq();
			if (msg == nullptr) break;
			
			if (msg->msg == Msg::NEW_CLI) {
				handle_new_client(msg);
				//msgNodeBuffer.retire(msg);
				continue;
			}

			if (my_clients[msg->to] == nullptr) {
				// ***gid�� ������ Ȯ�� �ʿ�
				//|| my_clients[msg->to]->gid)
				continue;
			}

			switch (msg->msg)
			{
			case Msg::MOVE: 
				handle_move_msg(msg);
				break;
			case Msg::HI:
				handle_hi_msg(msg);
				break;
			case Msg::BYE:
				handle_bye_msg(msg);
				break;
			default:
				cout << "UNKOWN MSG" << endl;
				break;
			}

			//msgNodeBuffer.retire(msg);
		}

		DWORD num_byte;
		ULONGLONG key64;
		PULONG_PTR p_key = &key64;
		WSAOVERLAPPED* p_over;
		BOOL no_error = GetQueuedCompletionStatus(g_iocp[tid], &num_byte, p_key, &p_over, 0);
		unsigned int key = static_cast<unsigned>(key64);

		if (FALSE == no_error) {
			int err_no = WSAGetLastError();
			if (ERROR_NETNAME_DELETED == err_no) {
				//Disconnect(key);
				continue;
			}
			else
				continue;
			//error_display("GQCS Error :", err_no);
		}

		OVER_EX* over_ex = reinterpret_cast<OVER_EX*> (p_over);
		switch (over_ex->event_type)
		{
		case EV_RECV:
			if (num_byte == 0) {
				Disconnect(key);
				continue;
			}
			handle_recv(key, over_ex, num_byte);
			break;
		case EV_SEND:
			sendBufferPool[tid].return_sendBuffer(over_ex);
			break;
		default:
			std::cout << "unknown Event error" << std::endl;
			break;
		}
		//++processed_io;
	}
}




int main()
{

	tid = NUM_WORKER_THREADS;
	epoch.store(1);
	for (int r = 0; r < NUM_WORKER_THREADS; ++r)
		reservations[r] = 0xffffffffffffffff;

	msg_node_epoch.store(1);
	for (int r = 0; r < NUM_WORKER_THREADS + 1; ++r)
		msg_node_reservations[r] = 0xffffffffffffffff;
	
	wcout.imbue(std::locale("korean"));

	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	SOCKET listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN serverAddr;
	memset(&serverAddr, 0, sizeof(SOCKADDR_IN));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	if (SOCKET_ERROR == ::bind(listenSocket, (struct sockaddr*) & serverAddr, sizeof(SOCKADDR_IN))) {
		error_display("WSARecv Error :", WSAGetLastError());
	}
	listen(listenSocket, 5);
	SOCKADDR_IN clientAddr;
	int addrLen = sizeof(SOCKADDR_IN);
	memset(&clientAddr, 0, addrLen);
	DWORD flags;

	
	for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
		g_iocp[i] = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	}

	// 2. Buffer �Ҵ� �� ���
	globalBufferPool.init();
	for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
		sendBufferPool[i].init();
	}


	vector <thread> worker_threads;
	for (int i = 0; i < NUM_WORKER_THREADS; ++i) worker_threads.emplace_back(do_worker, i);

	int cq_idx = 0;
	int global_id = 0;
	//4. Accept
	while (true) {
		if (mem_exceed_cnt.load() > NUM_WORKER_THREADS * 10) {
			cout << mem_exceed_cnt.load();
			continue;
		}
		//if (num_client.load(memory_order_acquire) >= 10)
		//	continue;
		SOCKET clientSocket = accept(listenSocket, (struct sockaddr*) & clientAddr, &addrLen);
		if (INVALID_SOCKET == clientSocket) {
			int err_no = WSAGetLastError();
			if (WSA_IO_PENDING != err_no)
				error_display("Accept Error :", err_no);
			continue;
		}
		
		//num_client.fetch_add(1, memory_order_release);
		
		cq_idx = ++cq_idx % NUM_WORKER_THREADS;
		SOCKETINFO* new_player = new SOCKETINFO;
		OVER_EX* recvBuf = globalBufferPool.get_recvBuffer();
		new_player->recv_over = recvBuf;
		new_player->x = rand() % WORLD_WIDTH;
		new_player->y = rand() % WORLD_HEIGHT;

		new_player->socket = clientSocket;
		new_player->prev_packet_size = 0;
		new_player->recv_buf_start_idx = 0;
		new_player->origin_offset = 0;

		new_player->is_connected = false;
		new_player->gid = global_id++;

		msgQueue[cq_idx].Enq(-1, -1, Msg::NEW_CLI, -1, -1, 0, -1, -1, new_player);
	}

	for (auto& th : worker_threads) th.join();
	closesocket(listenSocket);
	WSACleanup();
}