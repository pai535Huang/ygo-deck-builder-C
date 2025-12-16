#include "search_filter.h"
#include "image_loader.h"
#include "prerelease.h"
#include "offline_data.h"
#include "deck_io.h"
#include "dnd_manager.h"
#include "deck_slot.h"
#include "card_info.h"
#include <string.h>

// 全局变量：是否在搜索结果中显示先行卡（默认显示）
extern gboolean show_prerelease_cards;

// 外部函数声明
extern void list_clear(GtkListBox *list);
extern void draw_pixbuf_scaled(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
extern void on_drawing_area_destroy(GtkWidget *widget, gpointer user_data);
extern void free_card_preview(gpointer data);
extern void on_result_row_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
extern void on_result_row_released(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
extern void on_result_row_enter(GtkEventControllerMotion *controller, double x, double y, gpointer user_data);

// 逐个加载搜索结果图片，避免同时加载太多导致UI卡顿
gboolean search_load_next_image(gpointer user_data) {
    SearchUI *ui = (SearchUI*)user_data;
    
    // 检查队列是否仍然有效（可能在新搜索时被清理）
    // 注意：不要在这里释放队列，因为 on_search_clicked 会处理清理
    if (!ui || !ui->search_image_queue || ui->search_image_queue->len == 0) {
        if (ui) {
            ui->search_image_loader_id = 0;
        }
        return G_SOURCE_REMOVE;
    }
    
    // 每次加载 4 张图片以加快显示速度
    int batch_size = 4;
    int loaded = 0;
    
    while (loaded < batch_size && ui->search_image_queue && ui->search_image_queue->len > 0) {
        SearchImageToLoad *item = (SearchImageToLoad*)g_ptr_array_index(ui->search_image_queue, 0);
        
        if (!item) {
            g_ptr_array_remove_index(ui->search_image_queue, 0);
            continue;
        }
        
        int img_id = item->img_id;
        gboolean is_prerelease = item->is_prerelease;
        
        // 移除队列项（g_ptr_array_new_with_free_func 会自动释放 item）
        g_ptr_array_remove_index(ui->search_image_queue, 0);
        
        // 遍历搜索结果列表，查找对应的控件
        GtkWidget *list_child = gtk_widget_get_first_child(ui->list);
        GtkStack *stack = NULL;
        GtkWidget *target = NULL;
        
        while (list_child) {
            if (GTK_IS_LIST_BOX_ROW(list_child)) {
                GtkWidget *row_child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(list_child));
                if (row_child && GTK_IS_BOX(row_child)) {
                    // 查找行中的 stack
                    GtkWidget *box_child = gtk_widget_get_first_child(row_child);
                    while (box_child) {
                        if (GTK_IS_STACK(box_child)) {
                            // 找到 stack，检查其中的 picture
                            GtkWidget *picture = gtk_stack_get_child_by_name(GTK_STACK(box_child), "picture");
                            if (picture && GTK_IS_DRAWING_AREA(picture)) {
                                int stored_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picture), "pending_img_id"));
                                if (stored_id == img_id) {
                                    stack = GTK_STACK(box_child);
                                    target = picture;
                                    break;
                                }
                            }
                        }
                        box_child = gtk_widget_get_next_sibling(box_child);
                    }
                }
            }
            if (target) break;
            list_child = gtk_widget_get_next_sibling(list_child);
        }
        
        // 如果找到了对应的控件，加载图片
        if (target && stack) {
            // 清除标记，表示已处理
            g_object_set_data(G_OBJECT(target), "pending_img_id", GINT_TO_POINTER(0));
            
            if (is_prerelease) {
                // 从本地加载先行卡图片
                gchar *local_path = get_prerelease_card_image_path(img_id);
                if (local_path && g_file_test(local_path, G_FILE_TEST_EXISTS)) {
                    GError *error = NULL;
                    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(local_path, &error);
                    if (pixbuf) {
                        g_object_set_data_full(G_OBJECT(target), "pixbuf", pixbuf, (GDestroyNotify)g_object_unref);
                        g_object_set_data(G_OBJECT(target), "cached_surface", NULL);
                        gtk_widget_queue_draw(target);
                        gtk_stack_set_visible_child_name(stack, "picture");
                    } else {
                        if (error) g_error_free(error);
                    }
                    g_free(local_path);
                }
            } else {
                // 从在线加载普通卡片图片
                char url[128];
                g_snprintf(url, sizeof url, "https://cdn.233.momobako.com/ygoimg/jp/%d.webp", img_id);
                ImageLoadCtx *ctx = g_new0(ImageLoadCtx, 1);
                ctx->stack = stack;
                ctx->target = target;
                // 注意：不需要在这里设置弱引用，load_image_async 内部会处理
                ctx->scale_to_thumb = TRUE;
                ctx->cache_id = img_id;
                ctx->add_to_thumb_cache = TRUE;
                ctx->url = g_strdup(url);
                load_image_async(ui->session, url, ctx);
            }
            loaded++;
        }
    }
    
    return G_SOURCE_CONTINUE;
}

// 批量渲染回调：每次处理一批结果（10个）
gboolean batch_render_results(gpointer user_data) {
    SearchUI *ui = (SearchUI*)user_data;
    
    if (!ui || !ui->pending_results || ui->pending_results->len == 0) {
        if (ui) {
            ui->batch_render_id = 0;
        }
        return G_SOURCE_REMOVE;
    }
    
    // 每次处理20个结果，平衡性能和响应性
    const int BATCH_SIZE = 20;
    int processed = 0;
    
    while (processed < BATCH_SIZE && ui->pending_results && ui->pending_results->len > 0) {
        JsonObject *obj = g_ptr_array_index(ui->pending_results, 0);
        
        if (obj) {
            add_result_row_immediate(ui, obj);
        }
        
        // 移除队列项（g_ptr_array_new_with_free_func 会自动调用 json_object_unref）
        g_ptr_array_remove_index(ui->pending_results, 0);
        
        processed++;
    }
    
    // 如果队列已空，停止定时器并启动图片加载器
    if (!ui->pending_results || ui->pending_results->len == 0) {
        ui->batch_render_id = 0;
        
        // 批量渲染完成后，启动图片加载器
        if (ui->search_image_queue && ui->search_image_queue->len > 0 && ui->search_image_loader_id == 0) {
            ui->search_image_loader_id = g_timeout_add(20, search_load_next_image, ui);
        }
        
        return G_SOURCE_REMOVE;
    }
    
    return G_SOURCE_CONTINUE;
}

// 将结果加入渲染队列（延迟渲染）
void queue_result_for_render(SearchUI *ui, JsonObject *obj) {
    if (!ui) return;
    
    // 初始化队列
    if (!ui->pending_results) {
        ui->pending_results = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
    }
    
    // 增加引用计数并加入队列
    g_ptr_array_add(ui->pending_results, json_object_ref(obj));
    
    // 启动批量渲染定时器（使用高优先级idle，加快渲染速度）
    if (ui->batch_render_id == 0) {
        ui->batch_render_id = g_idle_add_full(G_PRIORITY_HIGH_IDLE, batch_render_results, ui, NULL);
    }
}

// 立即渲染单个结果行（原 add_result_row 函数）
void add_result_row_immediate(SearchUI *ui, JsonObject *obj) {
    const char *name = NULL;
    
    // 先检查是否有 text.name（先行卡格式）
    if (json_object_has_member(obj, "text")) {
        JsonObject *text = json_object_get_object_member(obj, "text");
        if (text && json_object_has_member(text, "name")) {
            name = json_object_get_string_member(text, "name");
        }
    }
    
    // 如果没有 text.name，按原顺序查找其他名称
    if (!name) name = json_object_get_string_member(obj, "cn_name");
    if (!name) name = json_object_get_string_member(obj, "sc_name");
    if (!name) name = json_object_get_string_member(obj, "jp_name");
    if (!name) name = json_object_get_string_member(obj, "en_name");
    
    int id = 0;
    if (json_object_has_member(obj, "id")) id = json_object_get_int_member(obj, "id");
    
    // 获取cid用于禁限卡表查询
    int cid = 0;
    if (json_object_has_member(obj, "cid")) {
        cid = json_object_get_int_member(obj, "cid");
    }

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);

    // 缩略图栈：占位与图片（强制 68x99 尺寸，避免单条结果时放大）
    GtkWidget *thumb_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(thumb_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(thumb_stack), 150);
    gtk_widget_set_size_request(thumb_stack, 68, 99);
    gtk_widget_set_halign(thumb_stack, GTK_ALIGN_START);
    gtk_widget_set_valign(thumb_stack, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(thumb_stack, FALSE);
    gtk_widget_set_vexpand(thumb_stack, FALSE);
    gtk_widget_add_css_class(thumb_stack, "thumb-fixed");
    // 占位框 68x99，居中Spinner
    GtkWidget *placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(placeholder, 68, 99);
    gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(placeholder, "thumb-fixed");
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_box_append(GTK_BOX(placeholder), spinner);
    gtk_stack_add_named(GTK_STACK(thumb_stack), placeholder, "placeholder");
    // 图片控件：使用 GtkDrawingArea + Cairo 高质量缩放绘制
    GtkWidget *picture = gtk_drawing_area_new();
    gtk_widget_set_size_request(picture, 68, 99);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(picture), draw_pixbuf_scaled, NULL, NULL);
    g_signal_connect(picture, "destroy", G_CALLBACK(on_drawing_area_destroy), NULL);
    gtk_widget_set_halign(picture, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(picture, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(picture, FALSE);
    gtk_widget_set_vexpand(picture, FALSE);
    gtk_widget_add_css_class(picture, "thumb-fixed");
    gtk_stack_add_named(GTK_STACK(thumb_stack), picture, "picture");
    gtk_stack_set_visible_child_name(GTK_STACK(thumb_stack), "placeholder");
    
    // 检查是否是先行卡
    gboolean is_prerelease = json_object_has_member(obj, "is_prerelease") && 
                             json_object_get_boolean_member(obj, "is_prerelease");
    
    if (id > 0) {
        // 不立即加载图片，而是添加到队列中
        // 先检查内存缓存，如果有则直接使用
        gboolean loaded_from_cache = FALSE;
        
        if (!is_prerelease) {
            GdkPixbuf *cached = get_thumb_from_cache(id);
            if (cached) {
                g_object_set_data_full(G_OBJECT(picture), "pixbuf", cached, (GDestroyNotify)g_object_unref);
                g_object_set_data(G_OBJECT(picture), "cached_surface", NULL);
                gtk_widget_queue_draw(picture);
                gtk_stack_set_visible_child_name(GTK_STACK(thumb_stack), "picture");
                loaded_from_cache = TRUE;
            }
        }

        // 内存缓存未命中时，也尝试从磁盘缓存读取（左栏预览会复用磁盘缓存）
        if (!loaded_from_cache && !is_prerelease) {
            GdkPixbuf *disk_cached = load_from_disk_cache(id);
            if (disk_cached) {
                g_object_set_data_full(G_OBJECT(picture), "pixbuf", disk_cached, (GDestroyNotify)g_object_unref);
                g_object_set_data(G_OBJECT(picture), "cached_surface", NULL);
                gtk_widget_queue_draw(picture);
                gtk_stack_set_visible_child_name(GTK_STACK(thumb_stack), "picture");
                loaded_from_cache = TRUE;
            }
        }
        
        // 如果没有从缓存加载，添加到加载队列
        if (!loaded_from_cache) {
            if (!ui->search_image_queue) {
                ui->search_image_queue = g_ptr_array_new_with_free_func(g_free);
            }
            
            // 在 picture 控件上存储待加载标记，用于后续查找
            g_object_set_data(G_OBJECT(picture), "pending_img_id", GINT_TO_POINTER(id));
            
            SearchImageToLoad *item = g_new0(SearchImageToLoad, 1);
            item->img_id = id;
            item->is_prerelease = is_prerelease;
            g_ptr_array_add(ui->search_image_queue, item);
        }
    }

    // 文字容器：固定宽度 208px（右栏300 - 左右边距16 - 图片68 - 间距8）
    // 防止文字区域扩展导致整行被拉伸
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_size_request(vbox, 208, -1);
    gtk_widget_set_hexpand(vbox, FALSE);
    gtk_widget_set_vexpand(vbox, FALSE);
    gtk_widget_set_halign(vbox, GTK_ALIGN_START);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *title = gtk_label_new(name ? name : "(无名)" );
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 20);

    // 从 text.types 获取类型字符串用于显示
    const char *types = NULL;
    if (json_object_has_member(obj, "text")) {
        JsonObject *text = json_object_get_object_member(obj, "text");
        if (text && json_object_has_member(text, "types")) types = json_object_get_string_member(text, "types");
    }
    GtkWidget *subtitle = gtk_label_new(types ? types : "");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(subtitle), 20);
    gtk_widget_add_css_class(subtitle, "dim-label");

    gtk_box_append(GTK_BOX(vbox), title);
    gtk_box_append(GTK_BOX(vbox), subtitle);
    
    // 显示禁限状态（使用cid而非id）
    if (cid > 0 && ui->forbidden_dropdown) {
        guint selected = gtk_drop_down_get_selected(ui->forbidden_dropdown);
        GHashTable *forbidden_table = NULL;
        
        if (selected == 0 && ui->ocg_forbidden) {
            forbidden_table = ui->ocg_forbidden;
        } else if (selected == 1 && ui->tcg_forbidden) {
            forbidden_table = ui->tcg_forbidden;
        } else if (selected == 2 && ui->sc_forbidden) {
            forbidden_table = ui->sc_forbidden;
        }
        
        if (forbidden_table) {
            char cid_str[32];
            g_snprintf(cid_str, sizeof(cid_str), "%d", cid);
            const char *status = g_hash_table_lookup(forbidden_table, cid_str);
            
            if (status) {
                // 创建带背景色的禁限状态标签
                GtkWidget *forbidden_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
                gtk_widget_set_halign(forbidden_box, GTK_ALIGN_START);
                
                GtkWidget *forbidden_label = gtk_label_new(NULL);
                char *markup = g_strdup_printf("<b>[%s]</b>", status);
                gtk_label_set_markup(GTK_LABEL(forbidden_label), markup);
                g_free(markup);
                
                gtk_label_set_xalign(GTK_LABEL(forbidden_label), 0.0);
                
                // 根据不同状态设置不同样式
                if (g_strcmp0(status, "禁止") == 0) {
                    gtk_widget_add_css_class(forbidden_label, "error");
                } else if (g_strcmp0(status, "限制") == 0) {
                    gtk_widget_add_css_class(forbidden_label, "warning");
                } else if (g_strcmp0(status, "准限制") == 0) {
                    gtk_widget_add_css_class(forbidden_label, "accent");
                }
                
                gtk_box_append(GTK_BOX(forbidden_box), forbidden_label);
                gtk_box_append(GTK_BOX(vbox), forbidden_box);
            }
        }
    }

    // 防止 row 扩展
    gtk_widget_set_hexpand(row, FALSE);
    gtk_widget_set_halign(row, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(row), thumb_stack);
    gtk_box_append(GTK_BOX(row), vbox);

    GtkWidget *list_row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row);
    // 绑定悬停事件
    GtkEventController *motion = GTK_EVENT_CONTROLLER(gtk_event_controller_motion_new());
    g_signal_connect(motion, "enter", G_CALLBACK(on_result_row_enter), ui);
    gtk_widget_add_controller(list_row, motion);
    // 绑定点击事件（与中栏一致：pressed 记录、released 判定点击）
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_result_row_pressed), ui);
    g_signal_connect(click, "released", G_CALLBACK(on_result_row_released), ui);
    gtk_widget_add_controller(list_row, GTK_EVENT_CONTROLLER(click));
    // 存储预览数据
    CardPreview *pv = g_new0(CardPreview, 1);
    pv->id = id;
    pv->cid = cid;
    pv->cn_name = name ? g_strdup(name) : NULL;
    pv->is_prerelease = is_prerelease;  // 设置是否是先行卡
    
    // 获取类型和其他 data 字段：先行卡的字段在顶层，普通卡的在 data 对象中
    if (is_prerelease) {
        // 先行卡的数据字段直接在顶层
        pv->type = json_object_has_member(obj, "type") ? (uint32_t)json_object_get_int_member(obj, "type") : 0;
        pv->ot = json_object_has_member(obj, "ot") ? json_object_get_int_member(obj, "ot") : -1;
        pv->setcode = json_object_has_member(obj, "setcode") ? json_object_get_int_member(obj, "setcode") : -1;
        pv->atk = json_object_has_member(obj, "atk") ? json_object_get_int_member(obj, "atk") : -1;
        pv->def = json_object_has_member(obj, "def") ? json_object_get_int_member(obj, "def") : -1;
        pv->level = json_object_has_member(obj, "level") ? json_object_get_int_member(obj, "level") : -1;
        pv->race = json_object_has_member(obj, "race") ? json_object_get_int_member(obj, "race") : -1;
        pv->attribute = json_object_has_member(obj, "attribute") ? json_object_get_int_member(obj, "attribute") : -1;
    } else if (json_object_has_member(obj, "data")) {
        // 普通卡的数据字段在 data 对象中
        JsonObject *data = json_object_get_object_member(obj, "data");
        if (data) {
            // 提取所有 data 字段
            if (json_object_has_member(data, "type")) {
                pv->type = (uint32_t)json_object_get_int_member(data, "type");
            }
            pv->ot = json_object_has_member(data, "ot") ? json_object_get_int_member(data, "ot") : -1;
            pv->setcode = json_object_has_member(data, "setcode") ? json_object_get_int_member(data, "setcode") : -1;
            pv->atk = json_object_has_member(data, "atk") ? json_object_get_int_member(data, "atk") : -1;
            pv->def = json_object_has_member(data, "def") ? json_object_get_int_member(data, "def") : -1;
            pv->level = json_object_has_member(data, "level") ? json_object_get_int_member(data, "level") : -1;
            pv->race = json_object_has_member(data, "race") ? json_object_get_int_member(data, "race") : -1;
            pv->attribute = json_object_has_member(data, "attribute") ? json_object_get_int_member(data, "attribute") : -1;
        }
    }
    
    if (json_object_has_member(obj, "text")) {
        JsonObject *text = json_object_get_object_member(obj, "text");
        if (text) {
            if (json_object_has_member(text, "types")) pv->types = g_strdup(json_object_get_string_member(text, "types"));
            if (json_object_has_member(text, "pdesc")) pv->pdesc = g_strdup(json_object_get_string_member(text, "pdesc"));
            if (json_object_has_member(text, "desc"))  pv->desc  = g_strdup(json_object_get_string_member(text, "desc"));
        }
    }
    g_object_set_data_full(G_OBJECT(list_row), "preview", pv, free_card_preview);
    // 右栏行支持拖拽到中栏：提供字符串 payload "search:<id>:<isExtra>"
    GtkDragSource *ds = gtk_drag_source_new();
    // 与中栏一致，采用 MOVE 动作，确保目标接受
    gtk_drag_source_set_actions(ds, GDK_ACTION_MOVE);
    g_signal_connect(ds, "prepare", G_CALLBACK(on_drag_prepare), NULL);
    g_signal_connect(ds, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    // 在行上存储一个标记供 prepare 使用
    g_object_set_data(G_OBJECT(list_row), "drag_kind", (gpointer)"search_row");
    // 为拖拽预览使用行内缩略图的 pixbuf（若可用）
    gtk_widget_add_controller(list_row, GTK_EVENT_CONTROLLER(ds));
    gtk_list_box_append(GTK_LIST_BOX(ui->list), list_row);
}

void search_response_cb(GObject *source, GAsyncResult *res, gpointer user_data) {
    SoupSession *session = SOUP_SESSION(source);
    SearchUI *ui = (SearchUI*)user_data;
    GError *err = NULL;
    GInputStream *in = soup_session_send_finish(session, res, &err);
    
    // 如果请求被取消（例如新搜索开始），直接返回
    if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return;
    }
    
    if (!in) { 
        if (err) g_error_free(err); 
        return; 
    }
    GByteArray *ba = g_byte_array_new();
    guint8 bufread[4096];
    gssize n;
    while ((n = g_input_stream_read(in, bufread, sizeof bufread, NULL, NULL)) > 0) {
        g_byte_array_append(ba, bufread, (guint)n);
    }
    JsonParser *parser = json_parser_new();
    GError *jerr = NULL;
    if (ba->len > 0 && json_parser_load_from_data(parser, (const char*)ba->data, (gssize)ba->len, &jerr)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_TYPE(root) == JSON_NODE_OBJECT) {
            JsonObject *robj = json_node_get_object(root);
            if (json_object_has_member(robj, "result")) {
                JsonArray *arr = json_object_get_array_member(robj, "result");
                guint nitems = json_array_get_length(arr);
                for (guint i = 0; i < nitems; ++i) {
                    JsonObject *item = json_array_get_object_element(arr, i);
                    if (item) queue_result_for_render(ui, item);
                }
            }
        }
    }
    if (jerr) g_error_free(jerr);
    g_object_unref(parser);
    g_byte_array_free(ba, TRUE);
    g_object_unref(in);
}

void on_forbidden_dropdown_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)dropdown;
    (void)pspec;
    SearchUI *ui = (SearchUI*)user_data;
    
    // 重新触发搜索以更新禁限状态显示
    const char *q = gtk_editable_get_text(GTK_EDITABLE(ui->entry));
    if (q && *q != '\0') {
        on_search_clicked(GTK_BUTTON(ui->button), ui);
    }
}

// 过滤选项回调函数
void on_monster_filter_toggled(GtkCheckButton *btn, gpointer user_data) {
    SearchUI *ui = (SearchUI*)user_data;
    ui->filter_by_monster = gtk_check_button_get_active(btn);
}

void on_spell_filter_toggled(GtkCheckButton *btn, gpointer user_data) {
    SearchUI *ui = (SearchUI*)user_data;
    ui->filter_by_spell = gtk_check_button_get_active(btn);
}

void on_trap_filter_toggled(GtkCheckButton *btn, gpointer user_data) {
    SearchUI *ui = (SearchUI*)user_data;
    ui->filter_by_trap = gtk_check_button_get_active(btn);
}

void on_search_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SearchUI *ui = (SearchUI*)user_data;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(ui->entry));
    
    // 如果搜索框为空且没有筛选条件，则不执行搜索
    if ((!q || *q == '\0') && !has_active_filter()) {
        return;
    }

    // 首先停止图片加载定时器，防止它在清理过程中访问数据
    if (ui->search_image_loader_id > 0) {
        g_source_remove(ui->search_image_loader_id);
        ui->search_image_loader_id = 0;
    }
    
    // 停止批量渲染定时器
    if (ui->batch_render_id > 0) {
        g_source_remove(ui->batch_render_id);
        ui->batch_render_id = 0;
    }
    
    // 清理待渲染队列
    GPtrArray *old_pending = ui->pending_results;
    ui->pending_results = NULL;
    if (old_pending) {
        g_ptr_array_free(old_pending, TRUE);
    }
    
    // 然后清理旧的图片加载队列
    // 先保存队列指针并清空 ui->search_image_queue，然后再释放
    GPtrArray *old_queue = ui->search_image_queue;
    ui->search_image_queue = NULL;
    if (old_queue) {
        g_ptr_array_free(old_queue, TRUE);
    }

    // 取消所有未完成的下载任务（通过递增代次）
    cancel_all_pending();
    
    // 在清空UI列表之前，标记所有现有行为"无效"
    // 这样即使有待处理的事件回调，它们也会检测到标记并跳过处理
    GtkWidget *list_child = gtk_widget_get_first_child(ui->list);
    while (list_child) {
        if (GTK_IS_LIST_BOX_ROW(list_child)) {
            g_object_set_data(G_OBJECT(list_child), "row_invalid", GINT_TO_POINTER(1));
        }
        list_child = gtk_widget_get_next_sibling(list_child);
    }
    
    // 最后清空UI列表（这会销毁widget并触发弱引用清理）
    list_clear(GTK_LIST_BOX(ui->list));

    // 获取当前筛选状态（在搜索先行卡之前就获取）
    const FilterState *filter = get_current_filter_state();
    
    // 判断是否需要搜索所有卡片（搜索框为空但有筛选条件）
    gboolean search_all = (!q || *q == '\0') && has_active_filter();
    
    // 设置最大结果数限制，避免UI卡死
    const guint MAX_RESULTS = 500;
    guint result_count = 0;

    // 如果启用了先行卡显示，先添加先行卡搜索结果
    if (show_prerelease_cards && result_count < MAX_RESULTS) {
        JsonArray *prerelease_results = search_all ? get_all_prerelease_cards() : search_prerelease_cards(q);
        if (prerelease_results) {
            guint len = json_array_get_length(prerelease_results);
            for (guint i = 0; i < len && result_count < MAX_RESULTS; i++) {
                JsonObject *item = json_array_get_object_element(prerelease_results, i);
                if (item) {
                    // 应用筛选条件
                    if (apply_filter(item, filter)) {
                        // 添加一个标记表示这是先行卡
                        JsonObject *marked_item = json_object_ref(item);
                        json_object_set_boolean_member(marked_item, "is_prerelease", TRUE);
                        queue_result_for_render(ui, marked_item);
                        json_object_unref(marked_item);
                        result_count++;
                    }
                }
            }
            json_array_unref(prerelease_results);
        }
    }

    // 检查是否启用离线数据
    gboolean offline_enabled = load_offline_data_switch_state();
    
    if (offline_enabled && offline_data_exists() && result_count < MAX_RESULTS) {
        // 使用离线数据搜索
        g_message("Searching in offline data...");
        JsonArray *offline_results = search_all ? get_all_offline_cards() : search_offline_cards(q);
        if (offline_results) {
            guint len = json_array_get_length(offline_results);
            g_message("Found %u cards in offline data", len);
            
            for (guint i = 0; i < len && result_count < MAX_RESULTS; i++) {
                JsonObject *item = json_array_get_object_element(offline_results, i);
                if (item) {
                    // 应用筛选条件
                    if (apply_filter(item, filter)) {
                        queue_result_for_render(ui, item);
                        result_count++;
                    }
                }
            }
            json_array_unref(offline_results);
            
            // 如果达到最大结果数，显示警告
            if (result_count >= MAX_RESULTS) {
                g_message("Reached maximum result limit (%u), stopping search", MAX_RESULTS);
            }
        } else {
            g_message("No results found in offline data");
        }
    } else if (!search_all && result_count < MAX_RESULTS) {
        // 仅在有搜索关键词时进行在线API搜索
        // 如果搜索框为空但有筛选条件，不进行在线搜索
        char *escaped = g_uri_escape_string(q, NULL, TRUE);
        char *url = g_strdup_printf("https://ygocdb.com/api/v0/?search=%s", escaped);
        g_free(escaped);

        SoupMessage *msg = soup_message_new("GET", url);
        g_free(url);
        if (!msg) return;
        // 搜索请求不使用 cancellable，让请求自然完成
        // 回调中会检查是否应该处理结果
        soup_session_send_async(ui->session, msg, G_PRIORITY_DEFAULT, NULL, search_response_cb, ui);
        g_object_unref(msg);  // soup_session_send_async 会内部增加引用
    } else {
        // 搜索框为空但有筛选条件，且离线数据未启用
        // 在这种情况下，只显示先行卡的筛选结果（如果有）
        g_message("Search query is empty with active filters, but offline data is not enabled");
    }
}
// 应用筛选条件到搜索结果
// 返回值：TRUE 表示卡片通过筛选，FALSE 表示应该被过滤掉
gboolean apply_filter(JsonObject *card, const FilterState *filter_state) {
    if (!card || !filter_state) {
        return TRUE;
    }
    
    // 判断卡片数据结构类型并缓存data对象（如果是离线数据）
    gboolean is_prerelease = json_object_has_member(card, "type");
    JsonObject *data = NULL;
    
    if (!is_prerelease) {
        if (json_object_has_member(card, "data")) {
            data = json_object_get_object_member(card, "data");
        }
    }
    
    // 定义辅助宏来获取卡片字段（先行卡或离线数据）
    #define GET_CARD_INT_FIELD(field_name, default_value) \
        (is_prerelease \
            ? (json_object_has_member(card, field_name) ? json_object_get_int_member(card, field_name) : (default_value)) \
            : (data && json_object_has_member(data, field_name) ? json_object_get_int_member(data, field_name) : (default_value)))
    
    // 首先检查字段筛选（不依赖卡片类型选择）
    if (filter_state->field_text && filter_state->field_text[0] != '\0') {
        // 获取卡片的setcode字段
        gint64 card_setcode = GET_CARD_INT_FIELD("setcode", 0);
        
        // 使用card_info中的函数检查字段匹配
        if (!match_setcode_with_field(card_setcode, filter_state->field_text)) {
            return FALSE;
        }
    }
    
    // 如果卡片类型选择是"全部"，不进行类型筛选
    if (filter_state->card_type_selected == 0) {
        return TRUE;
    }
    
    // 获取卡片的type字段
    gint64 card_type = 0;
    
    if (is_prerelease) {
        // 先行卡结构：直接有type字段
        card_type = json_object_get_int_member(card, "type");
    } else if (data && json_object_has_member(data, "type")) {
        // 离线数据结构：type在data对象中
        card_type = json_object_get_int_member(data, "type");
    } else {
        return FALSE;
    }
    
    // 怪兽卡筛选
    if (filter_state->card_type_selected == 1) {
        // 首先检查是否是怪兽卡
        if (!(card_type & 0x1)) {  // TYPE_MONSTER = 0x1
            return FALSE;
        }
        
        // 检查是否有任何怪兽类别被选中
        gboolean any_toggle_active = FALSE;
        for (int i = 0; i < 15; i++) {
            if (filter_state->monster_type_toggles[i]) {
                any_toggle_active = TRUE;
                break;
            }
        }
        
        // 如果有类别被选中，检查类别匹配
        if (any_toggle_active) {
            // 怪兽类别对应的type位（按照UI中的顺序）
            // "通常", "效果", "仪式", "融合", "同调", "超量", "灵摆", "连接", "调整",
            // "灵魂", "同盟", "二重", "反转", "卡通", "特殊召唤"
            const uint32_t type_flags[15] = {
                0x10,       // 通常 TYPE_NORMAL
                0x20,       // 效果 TYPE_EFFECT
                0x80,       // 仪式 TYPE_RITUAL
                0x40,       // 融合 TYPE_FUSION
                0x2000,     // 同调 TYPE_SYNCHRO
                0x800000,   // 超量 TYPE_XYZ
                0x1000000,  // 灵摆 TYPE_PENDULUM
                0x4000000,  // 连接 TYPE_LINK
                0x1000,     // 调整 TYPE_TUNER
                0x200,      // 灵魂 TYPE_SPIRIT
                0x400,      // 同盟 TYPE_UNION
                0x800,      // 二重 TYPE_DUAL
                0x200000,   // 反转 TYPE_FLIP
                0x400000,   // 卡通 TYPE_TOON
                0x2000000   // 特殊召唤 TYPE_SPSUMMON
            };
            
            // 构建目标type：所有选中的类别的OR组合
            uint32_t required_types = 0x1;  // 必须是怪兽
            for (int i = 0; i < 15; i++) {
                if (filter_state->monster_type_toggles[i]) {
                    required_types |= type_flags[i];
                }
            }
            
            // 检查卡片是否包含所有选中的类别
            gboolean match = ((card_type & required_types) == required_types);
            
            // 如果不匹配类别，直接返回
            if (!match) {
                return FALSE;
            }
        }
        
        // 检查连接箭头筛选（仅对连接怪兽有效）
        // 检查是否有连接箭头被选中
        gboolean any_link_marker_active = FALSE;
        for (int i = 0; i < 8; i++) {
            if (filter_state->link_marker_toggles[i]) {
                any_link_marker_active = TRUE;
                break;
            }
        }
        
        // 如果有连接箭头被选中，且卡片是连接怪兽，则检查箭头匹配
        if (any_link_marker_active && (card_type & 0x4000000)) {  // TYPE_LINK = 0x4000000
            // 连接箭头顺序：↖ ↑ ↗ ← → ↙ ↓ ↘
            const uint32_t link_marker_flags[8] = {
                0x040,  // ↖ LINK_MARKER_TOP_LEFT
                0x080,  // ↑ LINK_MARKER_TOP
                0x100,  // ↗ LINK_MARKER_TOP_RIGHT
                0x008,  // ← LINK_MARKER_LEFT
                0x020,  // → LINK_MARKER_RIGHT
                0x001,  // ↙ LINK_MARKER_BOTTOM_LEFT
                0x002,  // ↓ LINK_MARKER_BOTTOM
                0x004   // ↘ LINK_MARKER_BOTTOM_RIGHT
            };
            
            // 构建所需的连接箭头值
            uint32_t required_markers = 0;
            for (int i = 0; i < 8; i++) {
                if (filter_state->link_marker_toggles[i]) {
                    required_markers |= link_marker_flags[i];
                }
            }
            
            // 获取卡片的def字段（连接怪兽的def存储连接箭头）
            gint64 card_def = GET_CARD_INT_FIELD("def", 0);
            
            // 检查连接箭头是否完全匹配
            gboolean link_match = ((card_def & required_markers) == required_markers);
            
            return link_match;
        }
        
        // 检查灵摆刻度筛选（仅对灵摆怪兽有效）
        if (card_type & 0x1000000) {  // TYPE_PENDULUM = 0x1000000
            // 检查是否有左刻度或右刻度筛选条件
            gboolean has_left_scale_filter = (filter_state->left_scale_text && 
                                               filter_state->left_scale_text[0] != '\0');
            gboolean has_right_scale_filter = (filter_state->right_scale_text && 
                                                filter_state->right_scale_text[0] != '\0');
            
            if (has_left_scale_filter || has_right_scale_filter) {
                // 获取卡片的level字段
                gint64 card_level_field = GET_CARD_INT_FIELD("level", 0);
                
                // 解析灵摆刻度
                // level字段格式：第0-7位为等级，第16-23位为右刻度，第24-31位为左刻度
                int card_left_scale = (card_level_field >> 24) & 0xFF;
                int card_right_scale = (card_level_field >> 16) & 0xFF;
                
                // 检查左刻度
                if (has_left_scale_filter) {
                    int required_left_scale = atoi(filter_state->left_scale_text);
                    if (card_left_scale != required_left_scale) {
                        return FALSE;
                    }
                }
                
                // 检查右刻度
                if (has_right_scale_filter) {
                    int required_right_scale = atoi(filter_state->right_scale_text);
                    if (card_right_scale != required_right_scale) {
                        return FALSE;
                    }
                }
            }
        }
        
        // 检查属性筛选
        if (filter_state->attribute_selected != 0) {
            // 属性列表：全部(0), 地(1), 水(2), 炎(3), 风(4), 光(5), 暗(6), 神(7)
            const char *attributes[] = {
                "全部", "地", "水", "炎", "风", "光", "暗", "神"
            };
            
            const char *selected_attribute = NULL;
            if (filter_state->attribute_selected < 8) {
                selected_attribute = attributes[filter_state->attribute_selected];
            } else {
                selected_attribute = "全部";
            }
            
            uint32_t required_attribute = get_attribute_from_string(selected_attribute);
            
            if (required_attribute != 0) {
                // 获取卡片的attribute字段
                gint64 card_attribute = GET_CARD_INT_FIELD("attribute", 0);
                
                // 检查属性是否匹配
                if (card_attribute != required_attribute) {
                    return FALSE;
                }
            }
        }
        
        // 检查种族筛选
        if (filter_state->race_selected != 0) {
            // 种族列表按UI顺序
            const char *races[] = {
                "全部", "战士", "魔法师", "天使", "恶魔", "不死", "机械",
                "水", "炎", "岩石", "鸟兽", "植物", "昆虫", "雷", "龙", "兽",
                "兽战士", "恐龙", "鱼", "海龙", "爬虫类", "念动力",
                "幻神兽", "创造神", "幻龙", "电子界", "幻想魔"
            };
            
            const char *selected_race = NULL;
            if (filter_state->race_selected < 27) {
                selected_race = races[filter_state->race_selected];
            } else {
                selected_race = "全部";
            }
            
            uint32_t required_race = get_race_from_string(selected_race);
            
            if (required_race != 0) {
                // 获取卡片的race字段
                gint64 card_race = GET_CARD_INT_FIELD("race", 0);
                
                // 检查种族是否匹配
                if (card_race != required_race) {
                    return FALSE;
                }
            }
        }
        
        // 检查攻击力筛选
        if (filter_state->atk_text && filter_state->atk_text[0] != '\0') {
            int required_atk = atoi(filter_state->atk_text);
            
            // 获取卡片的atk字段
            gint64 card_atk = GET_CARD_INT_FIELD("atk", -1);
            
            // 检查攻击力是否匹配
            if (card_atk != required_atk) {
                return FALSE;
            }
        }
        
        // 检查守备力筛选
        // 注意：连接怪兽的def字段存储连接箭头，所以如果是连接怪兽则跳过守备力筛选
        gboolean is_link_monster = (card_type & 0x4000000) != 0;  // TYPE_LINK = 0x4000000
        
        if (!is_link_monster && filter_state->def_text && filter_state->def_text[0] != '\0') {
            int required_def = atoi(filter_state->def_text);
            
            // 获取卡片的def字段
            gint64 card_def = GET_CARD_INT_FIELD("def", -1);
            
            // 检查守备力是否匹配
            if (card_def != required_def) {
                return FALSE;
            }
        }
        
        // 检查等级筛选
        if (filter_state->level_text && filter_state->level_text[0] != '\0') {
            int required_level = atoi(filter_state->level_text);
            
            // 获取卡片的level字段
            gint64 card_level_field = GET_CARD_INT_FIELD("level", 0);
            
            // 从level字段中提取真正的等级（第0-7位）
            // 灵摆怪兽的level字段包含灵摆刻度信息：
            // 第0-7位为等级，第16-23位为右刻度，第24-31位为左刻度
            int card_level = card_level_field & 0xFF;
            
            // 检查等级是否匹配
            if (card_level != required_level) {
                return FALSE;
            }
        }
        
        return TRUE;
    }
    
    // 魔法卡筛选
    if (filter_state->card_type_selected == 2) {
        // 获取魔法类别对应的字符串
        const char *spell_categories[] = {
            "全部", "通常", "仪式", "速攻", "永续", "装备", "场地"
        };
        
        const char *selected_category = NULL;
        if (filter_state->spell_type_selected < 7) {
            selected_category = spell_categories[filter_state->spell_type_selected];
        } else {
            selected_category = "全部";
        }
        
        // 获取目标type值
        guint32 target_type = get_spell_type_from_category(selected_category);
        
        // 检查卡片type是否匹配
        // 使用精确匹配：卡片的type必须包含所有目标type的位
        gboolean match = ((card_type & target_type) == target_type);
        
        return match;
    }
    
    // 陷阱卡筛选
    if (filter_state->card_type_selected == 3) {
        // 获取陷阱类别对应的字符串
        const char *trap_categories[] = {
            "全部", "通常", "永续", "反击"
        };
        
        const char *selected_category = NULL;
        if (filter_state->trap_type_selected < 4) {
            selected_category = trap_categories[filter_state->trap_type_selected];
        } else {
            selected_category = "全部";
        }
        
        // 获取目标type值
        guint32 target_type = get_trap_type_from_category(selected_category);
        
        // 检查卡片type是否匹配
        gboolean match = ((card_type & target_type) == target_type);
        
        return match;
    }
    
    // 清理宏定义
    #undef GET_CARD_INT_FIELD
    
    // 其他类型暂不实现
    return TRUE;
}
