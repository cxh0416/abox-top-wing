#ifndef TIME_SYNC_H
#define TIME_SYNC_H
void TimeSync_Init(const char* device_id);
void TimeSync_Task(void);
const char* TimeSync_ResponseTopic(void);
int TimeSync_OnMessage(const char* topic, const char* payload);
#endif
