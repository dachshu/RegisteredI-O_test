#pragma once
#include <vector>
#include <iostream>
#include <mutex>
#include "Client.h"

class GlobalRecvBufferPool {
	std::vector<unsigned int> emptyRecvBufIdx;

	OVER_EX* recvOverBuffer;
	std::mutex recvBufLock;
public:
	GlobalRecvBufferPool() {

	}
	void init() {
		//2. Buffer 할당 및 등록
		// recv buffer
		recvOverBuffer = new OVER_EX[MAX_CLIENT];
		for (int i = 0; i < MAX_CLIENT; ++i) {
			memset(&recvOverBuffer[i].over, 0x00, sizeof(OVERLAPPED));
			recvOverBuffer[i].event_type = EV_RECV;
			recvOverBuffer[i].net_buf = new char[MAX_BUFFER];
			recvOverBuffer[i].wsabuf[0].buf = recvOverBuffer[i].net_buf;
			recvOverBuffer[i].wsabuf[0].len = MAX_BUFFER;
			recvOverBuffer[i].idx = i;
		}

		for (int i = MAX_CLIENT - 1; i >= 0; --i) {
			emptyRecvBufIdx.push_back(i);
		}
	}

	OVER_EX* get_recvBuffer() {
		std::lock_guard<std::mutex> lg(recvBufLock);
		if (emptyRecvBufIdx.empty())
			return nullptr;
		unsigned int idx = emptyRecvBufIdx.back();
		emptyRecvBufIdx.pop_back();
		return &recvOverBuffer[idx];
	}
	void return_recvBuffer(OVER_EX* ovex) {
		std::lock_guard<std::mutex> lg(recvBufLock);
		emptyRecvBufIdx.push_back(ovex->idx);
	}


};


class LocalSendBufferPool {
	std::vector<OVER_EX*> sendOverBuffers;
public:
	LocalSendBufferPool() {
	}
	void init() {
		// send buffer
		sendOverBuffers.reserve(int(LOCAL_SEND_BUF_CNT * 1.5));
		for (int i = 0; i < LOCAL_SEND_BUF_CNT; ++i) {
			OVER_EX* ov = new OVER_EX;
			memset(&ov->over, 0x00, sizeof(OVERLAPPED));
			ov->event_type = EV_SEND;
			ov->net_buf = new char[MAX_BUFFER];
			ov->wsabuf[0].len = MAX_BUFFER;
			ov->wsabuf[0].buf = ov->net_buf;

			sendOverBuffers.push_back(ov);
		}
	}

	void get_more_send_buffer() {

		for (int i = 0; i < LOCAL_SEND_BUF_CNT; ++i) {
			OVER_EX* ov = new OVER_EX;
			memset(&ov->over, 0x00, sizeof(OVERLAPPED));
			ov->event_type = EV_SEND;
			ov->net_buf = new char[MAX_BUFFER];
			ov->wsabuf[0].len = MAX_BUFFER;
			ov->wsabuf[0].buf = ov->net_buf;

			sendOverBuffers.push_back(ov);
		}

	}

	OVER_EX* get_sendBuffer() {
		if (sendOverBuffers.empty()) {
			std::cout << "no more send buf" << std::endl;
			get_more_send_buffer();
		}

		OVER_EX* ret = sendOverBuffers.back();
		sendOverBuffers.pop_back();
		return ret;
	}
	void return_sendBuffer(OVER_EX* ovex) {
		ovex->wsabuf[0].len = MAX_BUFFER;
		sendOverBuffers.push_back(ovex);
	}


};