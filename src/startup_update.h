#ifndef STARTUP_UPDATE_H
#define STARTUP_UPDATE_H

/**
 * 启动后台更新任务，下载OCG禁限卡表
 * 此函数会在后台线程中执行，不会阻塞主线程
 */
void startup_update_ocg_forbidden(void);

/**
 * 启动后台更新任务，下载TCG禁限卡表
 * 此函数会在后台线程中执行，不会阻塞主线程
 */
void startup_update_tcg_forbidden(void);

/**
 * 启动后台更新任务，下载SC禁限卡表
 * 此函数会在后台线程中执行，不会阻塞主线程
 */
void startup_update_sc_forbidden(void);

#endif // STARTUP_UPDATE_H
