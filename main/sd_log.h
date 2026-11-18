#pragma once

// TF 卡日志黑匣子 + 语音记文本

// 板级构造函数里立刻调用：分配环形缓冲、装日志钩子、起写盘任务。
// 写盘任务自己等 SD 挂载成功后才真正落盘，挂载失败就静默停摆。
void SdLogInitEarly();

// 服务器下发的 STT 原文进来，命中关键词就记一条备忘到卡上
void SdNotesOnUserText(const char* text);

// 记录模式：打开后不管有没有关键词，每句话都追加进当天文件
void SdNotesSetAlways(bool on);
bool SdNotesGetAlways();