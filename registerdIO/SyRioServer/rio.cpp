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



// max_client 만 받도록 처리필요
//atomic_int num_client{ 0 };




GlobalRecvBufferPool globalBufferPool;
LocalSendBufferPool sendBufferPool[NUM_WORKER_THREADS];
RIO_CQ compQueue[NUM_WORKER_THREADS];

const unsigned int zone_width = int(WORLD_WIDTH / ZONE_SIZE);
const unsigned int zone_heigt = int(WORLD_HEIGHT / ZONE_SIZE);
Zone zone[zone_heigt][zone_width];

const unsigned int per_max_clients = int(MAX_CLIENT / 2);
//thread_local int new_user_id = 0;
//thread_local unordered_map<int, SOCKETINFO*> my_clients;
thread_local SOCKETINFO* my_clients[per_max_clients];
thread_local std::set<int> used_cli_idx;
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
	wcout << L"에러 " << lpMsgBuf << endl;
	//while (true);
	LocalFree(lpMsgBuf);
}


bool is_near(int x1, int y1, int x2, int y2)
{
	if (VIEW_RANGE < abs(x1 - x2)) return false;
	if (VIEW_RANGE < abs(y1 - y2)) return false;
	return true;
}

void send_packet(RIO_RQ& rq, ExtendedRioBuf* send_buf, int cid)
{
	//char* packet = reinterpret_cast<char*>(buff);
	//int packet_size = packet[0];

	//ExtendedRioBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	//echo(packet, sendBuf, packet_size);
	if (my_clients[cid]->rem_send_cnt <= 0) {
		std::cout << "send ignored" << std::endl;
		sendBufferPool[tid].return_sendBuffer(send_buf);
		return;
	}
	
	if (false == gRIOFuncTable.RIOSend(rq, &(send_buf->rioBuf), 1, RIO_MSG_DEFER, (PVOID)send_buf)) {
		std::cout << "RIO send Error" << std::endl;
		int err_no = WSAGetLastError();
		error_display("RIO send Error :", err_no);
		//Disconnect(cid);
		//exit(1);
	}
	--(my_clients[cid]->rem_send_cnt);
}

void send_login_ok_packet(RIO_RQ& rq, int gid, int x, int y, int cid)
{
	ExtendedRioBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_login_ok* packet = reinterpret_cast<sc_packet_login_ok*>(sendBuf->buf_addr);

	packet->id = gid;
	packet->size = sizeof(sc_packet_login_ok);
	packet->type = SC_LOGIN_OK;
	packet->x = x;
	packet->y = y;
	packet->hp = 100;
	packet->level = 1;
	packet->exp = 1;
	sendBuf->rioBuf.Length = sizeof(sc_packet_login_ok);
	send_packet(rq, sendBuf, cid);
}


void send_login_fail(RIO_RQ& rq, int gid, int cid)
{
	ExtendedRioBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_login_fail* packet = reinterpret_cast<sc_packet_login_fail*>(sendBuf->buf_addr);

	packet->size = sizeof(sc_packet_login_fail);
	packet->type = SC_LOGIN_FAIL;
	sendBuf->rioBuf.Length = sizeof(sc_packet_login_fail);
	send_packet(rq, sendBuf, cid);
}

void send_put_object_packet(RIO_RQ& rq, int new_gid, int nx, int ny, int cid)
{
	ExtendedRioBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_put_object* packet = reinterpret_cast<sc_packet_put_object*>(sendBuf->buf_addr);

	packet->id = new_gid;
	packet->size = sizeof(sc_packet_put_object);
	packet->type = SC_PUT_OBJECT;
	packet->x = nx;
	packet->y = ny;
	packet->o_type = 1;
	sendBuf->rioBuf.Length = sizeof(sc_packet_put_object);
	send_packet(rq, sendBuf, cid);
}

void send_pos_packet(RIO_RQ& rq, int mover, int mx, int my, unsigned mv_time, int cid)
{
	ExtendedRioBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_pos* packet = reinterpret_cast<sc_packet_pos*>(sendBuf->buf_addr);
	packet->id = mover;
	packet->size = sizeof(sc_packet_pos);
	packet->type = SC_POS;
	packet->x = mx;
	packet->y = my;
	packet->move_time = mv_time;
	sendBuf->rioBuf.Length = sizeof(sc_packet_pos);

	send_packet(rq, sendBuf, cid);
}


void send_remove_object_packet(RIO_RQ& rq, int leaver, int cid)
{
	ExtendedRioBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_remove_object* packet = reinterpret_cast<sc_packet_remove_object*>(sendBuf->buf_addr);
	packet->id = leaver;
	packet->size = sizeof(sc_packet_remove_object);
	packet->type = SC_REMOVE_OBJECT;
	sendBuf->rioBuf.Length = sizeof(sc_packet_remove_object);
	send_packet(rq, sendBuf, cid);
}

void send_chat_packet(RIO_RQ& rq, int teller, char* mess, int cid)
{
	ExtendedRioBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_chat* packet = reinterpret_cast<sc_packet_chat*>(sendBuf->buf_addr);
	packet->id = teller;
	packet->size = sizeof(sc_packet_chat);
	packet->type = SC_CHAT;
	sendBuf->rioBuf.Length = sizeof(sc_packet_chat);
	send_packet(rq, sendBuf, cid);
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
	globalBufferPool.return_recvBuffer(my_clients[id]->recvBuf);

	delete my_clients[id];
	my_clients[id] = nullptr;
	used_cli_idx.erase(id);
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

	send_pos_packet(my_clients[id]->rq, my_clients[id]->gid, x, y, my_clients[id]->move_time, id);

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
	strcpy_s(my_clients[user_id]->name, MAX_STR_LEN, id_str);
	my_clients[user_id]->is_connected = true;
	send_login_ok_packet(my_clients[user_id]->rq, my_clients[user_id]->gid
		, my_clients[user_id]->x, my_clients[user_id]->y, user_id);
	send_put_object_packet(my_clients[user_id]->rq, my_clients[user_id]->gid
		, my_clients[user_id]->x, my_clients[user_id]->y, user_id);

	// 1. zone in11
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

void handle_recv(unsigned int uid, ExtendedRioBuf* riobuf, ULONG bytes) {
	int recv_buf_start_idx = my_clients[uid]->recv_buf_start_idx;
	char* p = &(riobuf->buf_addr[recv_buf_start_idx]);
	int remain = bytes;
	int packet_size;
	int prev_packet_size = my_clients[uid]->prev_packet_size;

	if (0 == prev_packet_size)
		packet_size = 0;
	else packet_size = my_clients[uid]->recvBuf->buf_addr[recv_buf_start_idx - prev_packet_size];
	
	int rem = 0;
	while (remain > 0) {
		if (0 == packet_size) packet_size = p[0];
		int required = packet_size - prev_packet_size;
		if (required <= remain) {
			//memcpy(my_clients[uid]->pre_net_buf + prev_packet_size, p, required);
			bool su = ProcessPacket(uid, &(riobuf->buf_addr[recv_buf_start_idx - prev_packet_size]));
			if (su == false) {
				Disconnect(uid);
				return;
			}
			remain -= required;
			p += required;
			recv_buf_start_idx += required;
			prev_packet_size = 0;
			packet_size = 0;
		}
		else {
			//memcpy(my_clients[uid]->pre_net_buf + prev_packet_size, p, remain);4
			memcpy(riobuf->buf_addr, &riobuf->buf_addr[recv_buf_start_idx - prev_packet_size], prev_packet_size);
			prev_packet_size += remain;
			recv_buf_start_idx = prev_packet_size;
			remain = 0;
			rem = 1;
		}
	}
	recv_buf_start_idx = rem * recv_buf_start_idx;
	
	my_clients[uid]->recv_buf_start_idx = recv_buf_start_idx;
	my_clients[uid]->prev_packet_size = prev_packet_size;
	
	my_clients[uid]->recvBuf->rioBuf.Offset = my_clients[uid]->origin_offset + recv_buf_start_idx;
	my_clients[uid]->recvBuf->rioBuf.Length = MAX_BUFFER - recv_buf_start_idx;

	if (false == gRIOFuncTable.RIOReceive(my_clients[uid]->rq, &(my_clients[uid]->recvBuf->rioBuf), 1,
		NULL, (PVOID)my_clients[uid]->recvBuf)) {
		std::cout << "RIO recv Error" << std::endl;
		//exit(1);
		Disconnect(uid);
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
		send_put_object_packet(my_clients[my_id]->rq, msg->gid, msg->x, msg->y, my_id);
		// ������� HI �����ֱ�
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::HI, mx, my, my_clients[my_id]->move_time, msg->from_cid, my_clients[my_id]->gid);
		return;
	}
	
	// 2. �˴� ��
	if (in_view && (my_clients[my_id]->near_id.count(msg->gid) != 0)) {
		// �׳� ������ send_pos_packet
		send_pos_packet(my_clients[my_id]->rq, msg->gid, msg->x, msg->y
			, msg->move_time, my_id);
		return;
	}

	// 3. ������� ��
	if (!in_view && (my_clients[my_id]->near_id.count(msg->gid) != 0)) {
		// list���� ���� send_remove_packet
		my_clients[my_id]->near_id.erase(msg->gid);
		send_remove_object_packet(my_clients[my_id]->rq, msg->gid, my_id);
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
		send_put_object_packet(my_clients[my_id]->rq, msg->gid, msg->x, msg->y, my_id);
	}
}

void handle_bye_msg(MsgNode* msg) {
	int my_id = msg->to;
	int mx = my_clients[my_id]->x;
	int my = my_clients[my_id]->y;

	if(my_clients[my_id]->near_id.count(msg->gid) != 0) {
		my_clients[my_id]->near_id.erase(msg->gid);
		send_remove_object_packet(my_clients[my_id]->rq, msg->gid, my_id);
	}
}


void handle_new_client(MsgNode* msg) {
	int user_id = empty_cli_idx[per_max_clients - 1 - num_my_clients];
	++num_my_clients;
	used_cli_idx.insert(user_id);

	SOCKETINFO* new_player = reinterpret_cast<SOCKETINFO*>(msg->info);
	new_player->idx = user_id;
	new_player->my_woker_id = tid;

	new_player->zone_node_buffer.set(new_player->my_woker_id, new_player->idx);

	// 5. RequestQueue 작성	
	new_player->rq = gRIOFuncTable.RIOCreateRequestQueue(new_player->socket,
		RECV_RQ_SIZE, 1, SEND_RQ_SIZE, 1,
		compQueue[tid], compQueue[tid], (PVOID)user_id);;

	my_clients[user_id] = new_player;
	//my_clients.insert(make_pair(user_id, new_player));

	// 6. RIO recv
	if (false == gRIOFuncTable.RIOReceive(new_player->rq
		, &(new_player->recvBuf->rioBuf), 1,
		NULL, (PVOID)(new_player->recvBuf))) {
		std::cout << "RIO recv Error" << std::endl;
		//exit(1);
		Disconnect(user_id);
	}



}

thread_local RIORESULT rioResults[MAX_RIO_REUSLTS];

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
				// ***gid도 같은지 확인 필요
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

		

		ULONG numResults;
		//7. RIODequeueComletion
		numResults = gRIOFuncTable.RIODequeueCompletion(compQueue[tid], rioResults, MAX_RIO_REUSLTS);

		if (RIO_CORRUPT_CQ == numResults) {
			std::cout << "Rio DQ error" << std::endl;
			//exit(1);
			continue;
		}


		for (ULONG i = 0; i < numResults; ++i) {
			ExtendedRioBuf* rioBuf = reinterpret_cast<ExtendedRioBuf*>(rioResults[i].RequestContext);
			unsigned int uid = (unsigned int)rioResults[i].SocketContext;
			ULONG bytes = rioResults[i].BytesTransferred;


			switch (rioBuf->event_type)
			{
			case EV_RECV:
				if (bytes == 0) {
					Disconnect(uid);
					break;
				}
				handle_recv(uid, rioBuf, bytes);
				break;
			case EV_SEND:
				if(my_clients[uid] != nullptr)
					++(my_clients[uid]->rem_send_cnt);
				sendBufferPool[tid].return_sendBuffer(rioBuf);
				break;
			default:
				std::cout << "unknown Event error" << std::endl;
				break;
			}
		}
		//processed_io += numResults;

		for (auto i : used_cli_idx) {
			gRIOFuncTable.RIOSend(my_clients[i]->rq, nullptr, 0, RIO_MSG_COMMIT_ONLY, nullptr);
		}
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
	SOCKET listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_REGISTERED_IO);
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

	

	// 1. RIO함수 table 설정
	GUID funcTableID = WSAID_MULTIPLE_RIO;
	DWORD dwBytes = 0;
	WSAIoctl(listenSocket, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,
		&funcTableID, sizeof(GUID),
		(void**)&gRIOFuncTable, sizeof(gRIOFuncTable),
		&dwBytes, NULL, NULL);

	// 2. Buffer 할당 및 등록
	globalBufferPool.init();
	for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
		sendBufferPool[i].init();
	}


	// 3. CompletionQueue 작성

	for (int i = 0; i < NUM_WORKER_THREADS; ++i) {

		compQueue[i] = gRIOFuncTable.RIOCreateCompletionQueue(long long((RECV_RQ_SIZE + SEND_RQ_SIZE) * (MAX_CLIENT / NUM_WORKER_THREADS) * 2), 0);
		if (compQueue[i] == RIO_INVALID_CQ) {
			std::cout << "create CQ error" << std::endl;
			exit(1);
		}
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
		ExtendedRioBuf* recvBuf = globalBufferPool.get_recvBuffer();
		new_player->recvBuf = recvBuf;
		new_player->x = rand() % WORLD_WIDTH;
		new_player->y = rand() % WORLD_HEIGHT;

		new_player->socket = clientSocket;
		new_player->prev_packet_size = 0;
		new_player->recv_buf_start_idx = 0;
		new_player->origin_offset = recvBuf->rioBuf.Offset;

		new_player->is_connected = false;
		new_player->gid = global_id++;

		msgQueue[cq_idx].Enq(-1, -1, Msg::NEW_CLI, -1, -1, 0, -1, -1, new_player);
	}

	for (auto& th : worker_threads) th.join();
	closesocket(listenSocket);
	WSACleanup();
}