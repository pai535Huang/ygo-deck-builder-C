#include "card_language.h"
#include "app_path.h"
#include "ui_language.h"

#include <glib/gstdio.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>

static const char *card_string(JsonObject *object, const char *key) {
    if (!object)
        return NULL;
    JsonNode *node = json_object_get_member(object, key);
    if (!node || json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;
    const char *value = json_node_get_string(node);
    return value && *value ? value : NULL;
}

static JsonObject *card_text(JsonObject *card) {
    if (!card)
        return NULL;
    JsonNode *node = json_object_get_member(card, "text");
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

const char *card_english_name(JsonObject *card) {
    const char *name = card_string(card, "en_name");
    return name ? name : card_string(card_text(card), "en_name");
}

const char *card_original_name(JsonObject *card) {
    const char *name = card_string(card_text(card), "name");
    if (name)
        return name;

    const char *fields[] = {"cn_name", "sc_name", "jp_name"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
        name = card_string(card, fields[i]);
        if (name)
            return name;
    }
    return card_english_name(card);
}

const char *card_name_for_language(const char *original, const char *english) {
    return ui_is_english() && english ? english : original;
}

// Translate category labels only. Names and effect text come from the card data.
static const struct {
    const char *original;
    const char *english;
} metadata_terms[] = {
    {"怪兽", "Monster"}, {"魔法", "Spell"}, {"陷阱", "Trap"},
    {"通常", "Normal"}, {"效果", "Effect"}, {"仪式", "Ritual"},
    {"融合", "Fusion"}, {"同调", "Synchro"}, {"超量", "Xyz"},
    {"灵摆", "Pendulum"}, {"连接", "Link"}, {"调整", "Tuner"},
    {"灵魂", "Spirit"}, {"同盟", "Union"}, {"二重", "Gemini"},
    {"反转", "Flip"}, {"卡通", "Toon"}, {"特殊召唤", "Special Summon"},
    {"衍生物", "Token"}, {"陷阱怪兽", "Trap Monster"},
    {"速攻", "Quick-Play"}, {"永续", "Continuous"}, {"装备", "Equip"},
    {"场地", "Field"}, {"反击", "Counter"},
    {"地", "EARTH"}, {"风", "WIND"}, {"光", "LIGHT"}, {"暗", "DARK"},
    {"神", "DIVINE"}, {"水", "Aqua"}, {"炎", "Pyro"},
    {"战士", "Warrior"}, {"魔法师", "Spellcaster"}, {"天使", "Fairy"},
    {"恶魔", "Fiend"}, {"不死", "Zombie"}, {"机械", "Machine"},
    {"岩石", "Rock"}, {"鸟兽", "Winged Beast"}, {"植物", "Plant"},
    {"昆虫", "Insect"}, {"雷", "Thunder"}, {"龙", "Dragon"}, {"兽", "Beast"},
    {"兽战士", "Beast-Warrior"}, {"恐龙", "Dinosaur"}, {"鱼", "Fish"},
    {"海龙", "Sea Serpent"}, {"爬虫类", "Reptile"}, {"念动力", "Psychic"},
    {"幻神兽", "Divine-Beast"}, {"创造神", "Creator God"}, {"幻龙", "Wyrm"},
    {"电子界", "Cyberse"}, {"幻想魔", "Illusion"},
    {"禁止", "Forbidden"}, {"限制", "Limited"}, {"准限制", "Semi-Limited"},
    {"无限制", "Unlimited"}, {"解除限制", "Unlimited"},
};

static gboolean translate_metadata(const GMatchInfo *match, GString *result, gpointer user_data) {
    const char *source = user_data;
    g_autofree char *token = g_match_info_fetch(match, 0);
    int start;
    g_match_info_fetch_pos(match, 0, &start, NULL);
    if (start > 0 && source[start - 1] == '/') {
        if (g_str_equal(token, "水")) {
            g_string_append(result, "WATER");
            return FALSE;
        }
        if (g_str_equal(token, "炎")) {
            g_string_append(result, "FIRE");
            return FALSE;
        }
    }

    for (guint i = 0; i < G_N_ELEMENTS(metadata_terms); i++) {
        if (g_str_equal(token, metadata_terms[i].original)) {
            g_string_append(result, metadata_terms[i].english);
            return FALSE;
        }
    }
    g_string_append(result, token);
    return FALSE;
}

char *card_display_metadata(const char *metadata) {
    if (!metadata || !ui_is_english())
        return g_strdup(metadata);

    static GRegex *tokens;
    if (!tokens)
        tokens = g_regex_new("[\\x{3400}-\\x{9fff}]+", G_REGEX_OPTIMIZE, 0, NULL);
    return g_regex_replace_eval(tokens, metadata, -1, 0, 0, translate_metadata,
                                (gpointer)metadata, NULL);
}

#define KONAMI_CARD_URL "https://www.db.yugioh-card.com/yugiohdb/card_search.action?ope=2&request_locale=en&cid=%d"
#define HAS_CLASS(name) "contains(concat(' ', normalize-space(@class), ' '), ' " name " ')"
#define CARD_TEXT_BOX "//div[" HAS_CLASS("CardText") "]"
#define EFFECT_TEXT "//div[" HAS_CLASS("text_linebreak") "]"
#define CARD_HTML_LIMIT (8 * 1024 * 1024)

static void append_html_text(GString *text, const xmlNode *node) {
    for (; node; node = node->next) {
        if (node->type == XML_TEXT_NODE)
            g_string_append(text, (const char *)node->content);
        else if (node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name, BAD_CAST "br"))
            g_string_append_c(text, '\n');
        else if (node->type == XML_ELEMENT_NODE)
            append_html_text(text, node->children);
    }
}

static char *get_html_text(xmlXPathContext *context, const char *path) {
    xmlXPathObject *result = xmlXPathEvalExpression(BAD_CAST path, context);
    if (!result)
        return NULL;

    char *value = NULL;
    if (result->nodesetval && result->nodesetval->nodeNr > 0) {
        GString *text = g_string_new(NULL);
        append_html_text(text, result->nodesetval->nodeTab[0]->children);
        value = g_strstrip(g_string_free(text, FALSE));
    }
    xmlXPathFreeObject(result);
    return value;
}

static GKeyFile *parse_official_card(GBytes *body) {
    gsize length;
    const char *data = g_bytes_get_data(body, &length);
    if (!length || length > CARD_HTML_LIMIT)
        return NULL;

    htmlDocPtr document = htmlReadMemory(data, (int)length, NULL, "UTF-8",
                                         HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!document)
        return NULL;

    GKeyFile *card = NULL;
    xmlXPathContext *context = xmlXPathNewContext(document);
    if (context) {
        g_autofree char *name = get_html_text(context, "//div[" HAS_CLASS("cardname") "]//h1");
        g_autofree char *effect = get_html_text(context,
            CARD_TEXT_BOX "[not(" HAS_CLASS("pen") ")]/div[" HAS_CLASS("item_box_text") "]" EFFECT_TEXT);
        g_autofree char *pendulum = get_html_text(context, CARD_TEXT_BOX "[" HAS_CLASS("pen") "]" EFFECT_TEXT);
        if (name && *name && effect) {
            card = g_key_file_new();
            g_key_file_set_string(card, "Card", "Name", name);
            g_key_file_set_string(card, "Card", "Text", effect);
            if (pendulum && *pendulum)
                g_key_file_set_string(card, "Card", "Pendulum", pendulum);
        }
        xmlXPathFreeContext(context);
    }
    xmlFreeDoc(document);
    return card;
}

static char *escape_effect_text(const char *text) {
    // The official page can encode line breaks as literal tags, including in cached text.
    static GRegex *line_breaks;
    if (!line_breaks)
        line_breaks = g_regex_new("<br\\s*/?>", G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
    g_autofree char *plain_text = g_regex_replace_literal(line_breaks, text, -1, 0, "\n", 0, NULL);
    return g_markup_escape_text(plain_text, -1);
}

static gboolean display_official_text(GtkLabel *label, GKeyFile *card, const char *metadata) {
    g_autofree char *name = g_key_file_get_string(card, "Card", "Name", NULL);
    g_autofree char *text = g_key_file_get_string(card, "Card", "Text", NULL);
    if (!name || !*name || !text)
        return FALSE;

    g_autofree char *pendulum = g_key_file_get_string(card, "Card", "Pendulum", NULL);
    g_autoptr(GString) markup = g_string_new(NULL);
    g_autofree char *heading = g_markup_printf_escaped("<b>%s</b>\n", name);
    g_string_append(markup, heading);
    if (metadata && *metadata) {
        g_autofree char *details = g_markup_printf_escaped("<small>%s</small>\n\n", metadata);
        g_string_append(markup, details);
    }
    if (pendulum && *pendulum) {
        g_autofree char *escaped_pendulum = escape_effect_text(pendulum);
        g_string_append_printf(markup, "<b>Pendulum Effect</b>\n%s\n\n", escaped_pendulum);
    }
    g_autofree char *escaped_text = escape_effect_text(text);
    g_string_append(markup, escaped_text);
    gtk_label_set_markup(label, markup->str);
    return TRUE;
}

typedef struct {
    GWeakRef label;
    SoupSession *session;
    GCancellable *cancellable;
    char *cache_path;
    char *metadata;
    int cid;
} OfficialCardRequest;

static void official_request_free(OfficialCardRequest *request) {
    g_weak_ref_clear(&request->label);
    g_object_unref(request->session);
    g_object_unref(request->cancellable);
    g_free(request->cache_path);
    g_free(request->metadata);
    g_free(request);
}

static void on_official_card_received(GObject *source, GAsyncResult *result, gpointer user_data) {
    OfficialCardRequest *request = user_data;
    SoupSession *session = SOUP_SESSION(source);
    g_autoptr(GBytes) body = soup_session_send_and_read_finish(session, result, NULL);
    SoupMessage *message = soup_session_get_async_result_message(session, result);
    if (body && message && SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(message)) &&
        !g_cancellable_is_cancelled(request->cancellable)) {
        g_autoptr(GKeyFile) card = parse_official_card(body);
        if (card) {
            g_autofree char *directory = g_path_get_dirname(request->cache_path);
            if (g_mkdir_with_parents(directory, 0755) == 0)
                g_key_file_save_to_file(card, request->cache_path, NULL);
            g_autoptr(GtkLabel) label = g_weak_ref_get(&request->label);
            if (label)
                display_official_text(label, card, request->metadata);
        }
    }
    official_request_free(request);
}

static gboolean request_official_card(gpointer user_data) {
    OfficialCardRequest *request = user_data;
    if (g_cancellable_is_cancelled(request->cancellable)) {
        official_request_free(request);
        return G_SOURCE_REMOVE;
    }

    g_autofree char *url = g_strdup_printf(KONAMI_CARD_URL, request->cid);
    g_autoptr(SoupMessage) message = soup_message_new("GET", url);
    soup_session_send_and_read_async(request->session, message, G_PRIORITY_LOW,
                                     request->cancellable, on_official_card_received, request);
    return G_SOURCE_REMOVE;
}

static void cancel_official_request(gpointer data) {
    g_cancellable_cancel(data);
    g_object_unref(data);
}

gboolean card_show_english_preview(GtkLabel *label, SoupSession *session, int cid,
                                   const char *metadata) {
    if (cid > 0 && cid == GPOINTER_TO_INT(g_object_get_data(G_OBJECT(label), "official-card-id")))
        return TRUE;

    // Changing the hovered card invalidates the previous response.
    g_object_set_data(G_OBJECT(label), "official-card-id", GINT_TO_POINTER(cid));
    g_object_set_data(G_OBJECT(label), "official-card-request", NULL);
    if (cid <= 0)
        return FALSE;

    g_autofree char *filename = g_strdup_printf("%d.conf", cid);
    g_autofree char *path = is_portable_mode()
        ? g_build_filename(get_program_directory(), "english-card-text", filename, NULL)
        : g_build_filename(g_get_user_cache_dir(), "ygo-deck-builder", "english-card-text", filename, NULL);
    g_autoptr(GKeyFile) cached = g_key_file_new();
    if (g_key_file_load_from_file(cached, path, G_KEY_FILE_NONE, NULL) &&
        display_official_text(label, cached, metadata))
        return TRUE;

    OfficialCardRequest *request = g_new0(OfficialCardRequest, 1);
    g_weak_ref_init(&request->label, label);
    request->session = g_object_ref(session);
    request->cancellable = g_cancellable_new();
    request->cache_path = g_steal_pointer(&path);
    request->metadata = g_strdup(metadata);
    request->cid = cid;
    g_object_set_data_full(G_OBJECT(label), "official-card-request",
                           g_object_ref(request->cancellable), cancel_official_request);
    g_timeout_add(250, request_official_card, request);
    return FALSE;
}
