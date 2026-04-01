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
 * 启动后台更新任务，下载AE禁限卡表
 * 此函数会在后台线程中执行，不会阻塞主线程
 */
void startup_update_ae_forbidden(void);

/**
 * 启动后台更新任务，下载SC禁限卡表
 * 此函数会在后台线程中执行，不会阻塞主线程
 */
void startup_update_sc_forbidden(void);

/**
 * 启动后台更新GENESYS分值表
 * 从 https://www.yugioh-card.com/en/genesys/ 获取数据
 * 此函数会在后台线程中执行，不会阻塞主线程
 */
void startup_update_genesys_forbidden(void);

/**
 * 启动后台更新OCG禁限卡表变更记录
 * 此函数会在后台线程中执行，不会阻塞主线程
 */
void startup_update_ocg_forbidden_changes(void);

/**
 * 启动后台更新TCG禁限卡表变更记录
 * 此函数会在后台线程中执行，不会阻塞主线程
 */
void startup_update_tcg_forbidden_changes(void);

/**
 * 启动后台更新AE禁限卡表变更记录
 * 此函数会在后台线程中执行，不会阻塞主线程
 */
void startup_update_ae_forbidden_changes(void);

/**
 * 启动后台更新SC禁限卡表变更记录
 * 此函数会在后台线程中执行，不会阻塞主线程
 */
void startup_update_sc_forbidden_changes(void);

#endif // STARTUP_UPDATE_H
