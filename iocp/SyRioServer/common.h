#pragma once
#include <iostream>
#include <WinSock2.h>
#include <atomic>

#define MAX_CLIENT			40000
#define MAX_BUFFER			128
#define NUM_WORKER_THREADS	8
#define LOCAL_SEND_BUF_CNT	int((MAX_CLIENT * 128))
#define INIT_NUM_ZONE_NODE		16

#define ZONE_SIZE			20
constexpr auto VIEW_RANGE = 7;

thread_local int tid;
std::atomic_uint mem_exceed_cnt{ 0 };
//thread_local int per_mem_exceed_cnt{ 0 };

#pragma comment(lib, "Ws2_32.lib")
