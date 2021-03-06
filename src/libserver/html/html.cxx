/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "util.h"
#include "message.h"
#include "html.h"
#include "html_tags.h"
#include "html_block.hxx"
#include "html.hxx"
#include "libserver/css/css_value.hxx"
#include "libserver/css/css.hxx"

#include "url.h"
#include "contrib/libucl/khash.h"
#include "libmime/images.h"
#include "libutil/cxx/utf8_util.h"

#include "html_tag_defs.hxx"
#include "html_entities.hxx"
#include "html_tag.hxx"
#include "html_url.hxx"

#include <vector>
#include <frozen/unordered_map.h>
#include <frozen/string.h>
#include <fmt/core.h>

#define DOCTEST_CONFIG_IMPLEMENTATION_IN_DLL
#include "doctest/doctest.h"


#include <unicode/uversion.h>

namespace rspamd::html {

static const guint max_tags = 8192; /* Ignore tags if this maximum is reached */

static const html_tags_storage html_tags_defs;

auto html_components_map = frozen::make_unordered_map<frozen::string, html_component_type>(
		{
				{"name", html_component_type::RSPAMD_HTML_COMPONENT_NAME},
				{"href", html_component_type::RSPAMD_HTML_COMPONENT_HREF},
				{"src", html_component_type::RSPAMD_HTML_COMPONENT_HREF},
				{"action", html_component_type::RSPAMD_HTML_COMPONENT_HREF},
				{"color", html_component_type::RSPAMD_HTML_COMPONENT_COLOR},
				{"bgcolor", html_component_type::RSPAMD_HTML_COMPONENT_BGCOLOR},
				{"style", html_component_type::RSPAMD_HTML_COMPONENT_STYLE},
				{"class", html_component_type::RSPAMD_HTML_COMPONENT_CLASS},
				{"width", html_component_type::RSPAMD_HTML_COMPONENT_WIDTH},
				{"height", html_component_type::RSPAMD_HTML_COMPONENT_HEIGHT},
				{"size", html_component_type::RSPAMD_HTML_COMPONENT_SIZE},
				{"rel", html_component_type::RSPAMD_HTML_COMPONENT_REL},
				{"alt", html_component_type::RSPAMD_HTML_COMPONENT_ALT},
				{"id", html_component_type::RSPAMD_HTML_COMPONENT_ID},
		});

#define msg_debug_html(...)  rspamd_conditional_debug_fast (NULL, NULL, \
        rspamd_html_log_id, "html", pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)

INIT_LOG_MODULE(html)

static auto
html_check_balance(struct html_tag *tag,
				   struct html_tag *parent,
				   std::vector<html_tag *> &tags_stack) -> bool
{

	if (tag->flags & FL_CLOSING) {
		/* Find the opening pair if any and check if it is correctly placed */
		auto found_opening = std::find_if(tags_stack.rbegin(), tags_stack.rend(),
				[&](const html_tag *t) {
					return (t->flags & FL_CLOSED) == 0 && t->id == tag->id;
				});

		if (found_opening != tags_stack.rend()) {
			(*found_opening)->flags |= FL_CLOSED;

			if (found_opening == tags_stack.rbegin()) {
				tags_stack.pop_back();
				/* All good */
				return true;
			}
			else {
				/* Move to front */
				std::iter_swap(found_opening, tags_stack.rbegin());
				tags_stack.pop_back();
				return true;
			}
		}
		else {
			/* We have unpaired tag */
			return false;
		}
	}

	/* Misuse */
	RSPAMD_UNREACHABLE;
}

static auto
html_process_tag(rspamd_mempool_t *pool,
				 struct html_content *hc,
				 struct html_tag *tag,
				 std::vector<html_tag *> &tags_stack) -> bool
{
	struct html_tag *parent;

	if (hc->total_tags > rspamd::html::max_tags) {
		hc->flags |= RSPAMD_HTML_FLAG_TOO_MANY_TAGS;
	}

	if (tag->id == -1) {
		/* Ignore unknown tags */
		hc->total_tags++;
		return false;
	}


	if (tags_stack.empty()) {
		parent = hc->root_tag;
	}
	else {
		parent = tags_stack.back();
	}

	tag->parent = parent;

	if (!(tag->flags & (CM_INLINE | CM_EMPTY))) {
		/* Block tag */
		if ((tag->flags & (FL_CLOSING | FL_CLOSED))) {
			/* Closed block tag */
			if (parent == nullptr) {
				msg_debug_html ("bad parent node");
				return false;
			}

			if (hc->total_tags < rspamd::html::max_tags) {
				if (!html_check_balance(tag, parent, tags_stack)) {
					msg_debug_html (
							"mark part as unbalanced as it has not pairable closing tags");
					hc->flags |= RSPAMD_HTML_FLAG_UNBALANCED;
				}

				hc->total_tags++;
			}
		}
		else {
			/* Opening block tag */
			if (parent) {
				if ((parent->flags & FL_IGNORE)) {
					tag->flags |= FL_IGNORE;
				}

				if (!(tag->flags & FL_CLOSED) &&
					!(parent->flags & FL_BLOCK)) {
					/* We likely have some bad nesting */
					if (parent->id == tag->id) {
						/* Something like <a>bla<a>foo... */
						hc->flags |= RSPAMD_HTML_FLAG_UNBALANCED;
						tag->parent = parent->parent;

						if (hc->total_tags < rspamd::html::max_tags) {
							parent->children.push_back(tag);
							tags_stack.push_back(tag);
							hc->total_tags++;
						}

						return true;
					}
				}

				if (hc->total_tags < rspamd::html::max_tags) {
					parent->children.push_back(tag);

					if ((tag->flags & FL_CLOSED) == 0) {
						tags_stack.push_back(tag);
					}

					hc->total_tags++;
				}
			}
			else {
				hc->root_tag = tag;
			}

			if (tag->flags & (CM_HEAD | CM_UNKNOWN | FL_IGNORE)) {
				tag->flags |= FL_IGNORE;

				return false;
			}

		}
	}
	else {
		/* Inline tag */
		if (parent) {
			if (hc->total_tags < rspamd::html::max_tags) {
				parent->children.push_back(tag);

				hc->total_tags++;
			}
			if ((parent->flags & (CM_HEAD | CM_UNKNOWN | FL_IGNORE))) {
				tag->flags |= FL_IGNORE;

				return false;
			}
		}
	}

	return true;
}

auto
html_component_from_string(const std::string_view &st) -> std::optional<html_component_type>
{
	auto known_component_it = html_components_map.find(st);

	if (known_component_it != html_components_map.end()) {
		return known_component_it->second;
	}
	else {
		return std::nullopt;
	}
}

static auto
find_tag_component_name(rspamd_mempool_t *pool,
					const gchar *begin,
					const gchar *end) -> std::optional<html_component_type>
{
	if (end <= begin) {
		return std::nullopt;
	}

	auto *p = rspamd_mempool_alloc_buffer(pool, end - begin);
	memcpy(p, begin, end - begin);
	auto len = decode_html_entitles_inplace(p, end - begin);
	len = rspamd_str_lc(p, len);
	auto known_component_it = html_components_map.find({p, len});

	if (known_component_it != html_components_map.end()) {
		return known_component_it->second;
	}
	else {
		return std::nullopt;
	}
}

struct tag_content_parser_state {
	int cur_state = 0;
	const char *saved_p = nullptr;
	std::optional<html_component_type> cur_component;

	void reset()
	{
		cur_state = 0;
		saved_p = nullptr;
		cur_component = std::nullopt;
	}
};

static inline void
html_parse_tag_content(rspamd_mempool_t *pool,
					   struct html_content *hc,
					   struct html_tag *tag,
					   const char *in,
					   struct tag_content_parser_state &parser_env)
{
	enum tag_parser_state {
		parse_start = 0,
		parse_name,
		parse_attr_name,
		parse_equal,
		parse_start_dquote,
		parse_dqvalue,
		parse_end_dquote,
		parse_start_squote,
		parse_sqvalue,
		parse_end_squote,
		parse_value,
		spaces_after_name,
		spaces_before_eq,
		spaces_after_eq,
		spaces_after_param,
		ignore_bad_tag
	} state;
	gboolean store = FALSE;

	state = static_cast<enum tag_parser_state>(parser_env.cur_state);

	/*
	 * Stores tag component if it doesn't exist, performing copy of the
	 * value + decoding of the entities
	 * Parser env is set to clear the current html attribute fields (saved_p and
	 * cur_component)
	 */
	auto store_tag_component = [&]() -> void {
		if (parser_env.saved_p != nullptr && parser_env.cur_component &&
			in > parser_env.saved_p) {

			/* We ignore repeated attributes */
				auto sz = (std::size_t)(in - parser_env.saved_p);
				auto *s = rspamd_mempool_alloc_buffer(pool, sz);
				memcpy(s, parser_env.saved_p, sz);
				sz = rspamd_html_decode_entitles_inplace(s, in - parser_env.saved_p);
				tag->parameters.emplace_back(parser_env.cur_component.value(),
						std::string_view{s, sz});
		}

		parser_env.saved_p = nullptr;
		parser_env.cur_component = std::nullopt;
	};

	switch (state) {
	case parse_start:
		if (!g_ascii_isalpha (*in) && !g_ascii_isspace (*in)) {
			hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
			state = ignore_bad_tag;
			tag->id = -1;
			tag->flags |= FL_BROKEN;
		}
		else if (g_ascii_isalpha (*in)) {
			state = parse_name;
			tag->name = std::string_view{in, 0};
		}
		break;

	case parse_name:
		if (g_ascii_isspace (*in) || *in == '>' || *in == '/') {
			const auto *start = tag->name.begin();
			g_assert (in >= start);

			if (*in == '/') {
				tag->flags |= FL_CLOSED;
			}

			tag->name = std::string_view{start, (std::size_t)(in - start)};

			if (tag->name.empty()) {
				hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
				tag->id = -1;
				tag->flags |= FL_BROKEN;
				state = ignore_bad_tag;
			}
			else {
				/*
				 * Copy tag name to the temporary buffer for modifications
				 */
				auto *s = rspamd_mempool_alloc_buffer(pool, tag->name.size() + 1);
				rspamd_strlcpy(s, tag->name.data(), tag->name.size() + 1);
				auto nsize = rspamd_html_decode_entitles_inplace(s,
						tag->name.size());
				nsize =  rspamd_str_lc_utf8(s, nsize);
				tag->name = std::string_view{s, nsize};

				const auto *tag_def = rspamd::html::html_tags_defs.by_name(tag->name);

				if (tag_def == nullptr) {
					hc->flags |= RSPAMD_HTML_FLAG_UNKNOWN_ELEMENTS;
					tag->id = -1;
				}
				else {
					tag->id = tag_def->id;
					tag->flags = tag_def->flags;
				}

				state = spaces_after_name;
			}
		}
		break;

	case parse_attr_name:
		if (parser_env.saved_p == nullptr) {
			state = ignore_bad_tag;
		}
		else {
			const auto *attr_name_end = in;

			if (*in == '=') {
				state = parse_equal;
			}
			else if (*in == '"') {
				/* No equal or something sane but we have quote character */
				state = parse_start_dquote;
				attr_name_end = in - 1;

				while (attr_name_end > parser_env.saved_p) {
					if (!g_ascii_isalnum (*attr_name_end)) {
						attr_name_end--;
					}
					else {
						break;
					}
				}

				/* One character forward to obtain length */
				attr_name_end++;
			}
			else if (g_ascii_isspace (*in)) {
				state = spaces_before_eq;
			}
			else if (*in == '/') {
				tag->flags |= FL_CLOSED;
			}
			else if (!g_ascii_isgraph (*in)) {
				state = parse_value;
				attr_name_end = in - 1;

				while (attr_name_end > parser_env.saved_p) {
					if (!g_ascii_isalnum (*attr_name_end)) {
						attr_name_end--;
					}
					else {
						break;
					}
				}

				/* One character forward to obtain length */
				attr_name_end++;
			}
			else {
				return;
			}

			parser_env.cur_component = find_tag_component_name(pool,
					parser_env.saved_p,
					attr_name_end);

			if (!parser_env.cur_component) {
				/* Ignore unknown params */
				parser_env.saved_p = nullptr;
			}
			else if (state == parse_value) {
				parser_env.saved_p = in + 1;
			}
		}

		break;

	case spaces_after_name:
		if (!g_ascii_isspace (*in)) {
			parser_env.saved_p = in;

			if (*in == '/') {
				tag->flags |= FL_CLOSED;
			}
			else if (*in != '>') {
				state = parse_attr_name;
			}
		}
		break;

	case spaces_before_eq:
		if (*in == '=') {
			state = parse_equal;
		}
		else if (!g_ascii_isspace (*in)) {
			/*
			 * HTML defines that crap could still be restored and
			 * calculated somehow... So we have to follow this stupid behaviour
			 */
			/*
			 * TODO: estimate what insane things do email clients in each case
			 */
			if (*in == '>') {
				/*
				 * Attribtute name followed by end of tag
				 * Should be okay (empty attribute). The rest is handled outside
				 * this automata.
				 */

			}
			else if (*in == '"' || *in == '\'') {
				/* Attribute followed by quote... Missing '=' ? Dunno, need to test */
				hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
				tag->flags |= FL_BROKEN;
				state = ignore_bad_tag;
			}
			else {
				/*
				 * Just start another attribute ignoring an empty attributes for
				 * now. We don't use them in fact...
				 */
				state = parse_attr_name;
				parser_env.saved_p = in;
			}
		}
		break;

	case spaces_after_eq:
		if (*in == '"') {
			state = parse_start_dquote;
		}
		else if (*in == '\'') {
			state = parse_start_squote;
		}
		else if (!g_ascii_isspace (*in)) {
			if (parser_env.saved_p != nullptr) {
				/* We need to save this param */
				parser_env.saved_p = in;
			}
			state = parse_value;
		}
		break;

	case parse_equal:
		if (g_ascii_isspace (*in)) {
			state = spaces_after_eq;
		}
		else if (*in == '"') {
			state = parse_start_dquote;
		}
		else if (*in == '\'') {
			state = parse_start_squote;
		}
		else {
			if (parser_env.saved_p != nullptr) {
				/* We need to save this param */
				parser_env.saved_p = in;
			}
			state = parse_value;
		}
		break;

	case parse_start_dquote:
		if (*in == '"') {
			if (parser_env.saved_p != nullptr) {
				/* We have an empty attribute value */
				parser_env.saved_p = nullptr;
			}
			state = spaces_after_param;
		}
		else {
			if (parser_env.saved_p != nullptr) {
				/* We need to save this param */
				parser_env.saved_p = in;
			}
			state = parse_dqvalue;
		}
		break;

	case parse_start_squote:
		if (*in == '\'') {
			if (parser_env.saved_p != nullptr) {
				/* We have an empty attribute value */
				parser_env.saved_p = nullptr;
			}
			state = spaces_after_param;
		}
		else {
			if (parser_env.saved_p != nullptr) {
				/* We need to save this param */
				parser_env.saved_p = in;
			}
			state = parse_sqvalue;
		}
		break;

	case parse_dqvalue:
		if (*in == '"') {
			store = TRUE;
			state = parse_end_dquote;
		}

		if (store) {
			store_tag_component();
		}
		break;

	case parse_sqvalue:
		if (*in == '\'') {
			store = TRUE;
			state = parse_end_squote;
		}
		if (store) {
			store_tag_component();
		}
		break;

	case parse_value:
		if (*in == '/' && *(in + 1) == '>') {
			tag->flags |= FL_CLOSED;
			store = TRUE;
		}
		else if (g_ascii_isspace (*in) || *in == '>' || *in == '"') {
			store = TRUE;
			state = spaces_after_param;
		}

		if (store) {
			store_tag_component();
		}
		break;

	case parse_end_dquote:
	case parse_end_squote:
		if (g_ascii_isspace (*in)) {
			state = spaces_after_param;
		}
		else if (*in == '/' && *(in + 1) == '>') {
			tag->flags |= FL_CLOSED;
		}
		else {
			/* No space, proceed immediately to the attribute name */
			state = parse_attr_name;
			parser_env.saved_p = in;
		}
		break;

	case spaces_after_param:
		if (!g_ascii_isspace (*in)) {
			if (*in == '/' && *(in + 1) == '>') {
				tag->flags |= FL_CLOSED;
			}

			state = parse_attr_name;
			parser_env.saved_p = in;
		}
		break;

	case ignore_bad_tag:
		break;
	}

	parser_env.cur_state = state;
}

static auto
html_process_url_tag(rspamd_mempool_t *pool,
					 struct html_tag *tag,
					 struct html_content *hc) -> std::optional<struct rspamd_url *>
{
	auto found_href_maybe = tag->find_component(html_component_type::RSPAMD_HTML_COMPONENT_HREF);

	if (found_href_maybe) {
		/* Check base url */
		auto &href_value = found_href_maybe.value();

		if (hc && hc->base_url && href_value.size() > 2) {
			/*
			 * Relative url cannot start from the following:
			 * schema://
			 * data:
			 * slash
			 */

			if (rspamd_substring_search(href_value.data(), href_value.size(), "://", 3) == -1) {

				if (href_value.size() >= sizeof("data:") &&
					g_ascii_strncasecmp(href_value.data(), "data:", sizeof("data:") - 1) == 0) {
					/* Image data url, never insert as url */
					return std::nullopt;
				}

				/* Assume relative url */
				auto need_slash = false;

				auto orig_len = href_value.size();
				auto len = orig_len + hc->base_url->urllen;

				if (hc->base_url->datalen == 0) {
					need_slash = true;
					len++;
				}

				auto *buf = rspamd_mempool_alloc_buffer(pool, len + 1);
				auto nlen = (std::size_t)rspamd_snprintf(buf, len + 1,
						"%*s%s%*s",
						hc->base_url->urllen, hc->base_url->string,
						need_slash ? "/" : "",
						(gint) orig_len, href_value.size());
				href_value = {buf, nlen};
			}
			else if (href_value[0] == '/' && href_value[1] != '/') {
				/* Relative to the hostname */
				auto orig_len = href_value.size();
				auto len = orig_len + hc->base_url->hostlen + hc->base_url->protocollen +
					   3 /* for :// */;
				auto *buf = rspamd_mempool_alloc_buffer(pool, len + 1);
				auto nlen = (std::size_t)rspamd_snprintf(buf, len + 1, "%*s://%*s/%*s",
						hc->base_url->protocollen, hc->base_url->string,
						hc->base_url->hostlen, rspamd_url_host_unsafe (hc->base_url),
						(gint)orig_len, href_value.data());
				href_value = {buf, nlen};
			}
		}

		auto url = html_process_url(pool, href_value);

		if (url && std::holds_alternative<std::monostate>(tag->extra)) {
			tag->extra = url.value();
		}

		return url;
	}

	return std::nullopt;
}

struct rspamd_html_url_query_cbd {
	rspamd_mempool_t *pool;
	khash_t (rspamd_url_hash) *url_set;
	struct rspamd_url *url;
	GPtrArray *part_urls;
};

static gboolean
html_url_query_callback(struct rspamd_url *url, gsize start_offset,
							   gsize end_offset, gpointer ud)
{
	struct rspamd_html_url_query_cbd *cbd =
			(struct rspamd_html_url_query_cbd *) ud;
	rspamd_mempool_t *pool;

	pool = cbd->pool;

	if (url->protocol == PROTOCOL_MAILTO) {
		if (url->userlen == 0) {
			return FALSE;
		}
	}

	msg_debug_html ("found url %s in query of url"
					" %*s", url->string,
			cbd->url->querylen, rspamd_url_query_unsafe(cbd->url));

	url->flags |= RSPAMD_URL_FLAG_QUERY;

	if (rspamd_url_set_add_or_increase(cbd->url_set, url, false)
		&& cbd->part_urls) {
		g_ptr_array_add(cbd->part_urls, url);
	}

	return TRUE;
}

static void
html_process_query_url(rspamd_mempool_t *pool, struct rspamd_url *url,
					   khash_t (rspamd_url_hash) *url_set,
					   GPtrArray *part_urls)
{
	if (url->querylen > 0) {
		struct rspamd_html_url_query_cbd qcbd;

		qcbd.pool = pool;
		qcbd.url_set = url_set;
		qcbd.url = url;
		qcbd.part_urls = part_urls;

		rspamd_url_find_multiple(pool,
				rspamd_url_query_unsafe (url), url->querylen,
				RSPAMD_URL_FIND_ALL, NULL,
				html_url_query_callback, &qcbd);
	}

	if (part_urls) {
		g_ptr_array_add(part_urls, url);
	}
}

static auto
html_process_data_image(rspamd_mempool_t *pool,
						struct html_image *img,
						std::string_view input) -> void
{
	/*
	 * Here, we do very basic processing of the data:
	 * detect if we have something like: `data:image/xxx;base64,yyyzzz==`
	 * We only parse base64 encoded data.
	 * We ignore content type so far
	 */
	struct rspamd_image *parsed_image;
	const gchar *semicolon_pos = input.data(),
			*end = input.data() + input.size();

	if ((semicolon_pos = (const gchar *)memchr(semicolon_pos, ';', end - semicolon_pos)) != NULL) {
		if (end - semicolon_pos > sizeof("base64,")) {
			if (memcmp(semicolon_pos + 1, "base64,", sizeof("base64,") - 1) == 0) {
				const gchar *data_pos = semicolon_pos + sizeof("base64,");
				gchar *decoded;
				gsize encoded_len = end - data_pos, decoded_len;
				rspamd_ftok_t inp;

				decoded_len = (encoded_len / 4 * 3) + 12;
				decoded = rspamd_mempool_alloc_buffer(pool, decoded_len);
				rspamd_cryptobox_base64_decode(data_pos, encoded_len,
						reinterpret_cast<guchar *>(decoded), &decoded_len);
				inp.begin = decoded;
				inp.len = decoded_len;

				parsed_image = rspamd_maybe_process_image(pool, &inp);

				if (parsed_image) {
					msg_debug_html ("detected %s image of size %ud x %ud in data url",
							rspamd_image_type_str(parsed_image->type),
							parsed_image->width, parsed_image->height);
					img->embedded_image = parsed_image;
				}
			}
		}
		else {
			/* Nothing useful */
			return;
		}
	}
}

static void
html_process_img_tag(rspamd_mempool_t *pool,
					 struct html_tag *tag,
					 struct html_content *hc,
					 khash_t (rspamd_url_hash) *url_set,
					 GPtrArray *part_urls)
{
	struct html_image *img;

	img = rspamd_mempool_alloc0_type (pool, struct html_image);
	img->tag = tag;
	tag->flags |= FL_IMAGE;


	for (const auto &param : tag->parameters) {

		if (param.type == html_component_type::RSPAMD_HTML_COMPONENT_HREF) {
			/* Check base url */
			const auto &href_value = param.value;

			if (href_value.size() > 0) {
				rspamd_ftok_t fstr;
				fstr.begin = href_value.data();
				fstr.len = href_value.size();
				img->src = rspamd_mempool_ftokdup (pool, &fstr);

				if (href_value.size() > sizeof("cid:") - 1 && memcmp(href_value.data(),
						"cid:", sizeof("cid:") - 1) == 0) {
					/* We have an embedded image */
					img->flags |= RSPAMD_HTML_FLAG_IMAGE_EMBEDDED;
				}
				else {
					if (href_value.size() > sizeof("data:") - 1 && memcmp(href_value.data(),
							"data:", sizeof("data:") - 1) == 0) {
						/* We have an embedded image in HTML tag */
						img->flags |=
								(RSPAMD_HTML_FLAG_IMAGE_EMBEDDED | RSPAMD_HTML_FLAG_IMAGE_DATA);
						html_process_data_image(pool, img, href_value);
						hc->flags |= RSPAMD_HTML_FLAG_HAS_DATA_URLS;
					}
					else {
						img->flags |= RSPAMD_HTML_FLAG_IMAGE_EXTERNAL;
						if (img->src) {

							std::string_view cpy{href_value};
							auto maybe_url = html_process_url(pool, cpy);

							if (maybe_url) {
								img->url = maybe_url.value();
								struct rspamd_url *existing;

								img->url->flags |= RSPAMD_URL_FLAG_IMAGE;
								existing = rspamd_url_set_add_or_return(url_set, img->url);

								if (existing != img->url) {
									/*
									 * We have some other URL that could be
									 * found, e.g. from another part. However,
									 * we still want to set an image flag on it
									 */
									existing->flags |= img->url->flags;
									existing->count++;
								}
								else if (part_urls) {
									/* New url */
									g_ptr_array_add(part_urls, img->url);
								}
							}
						}
					}
				}
			}
		}


		if (param.type == html_component_type::RSPAMD_HTML_COMPONENT_HEIGHT) {
			unsigned long val;

			rspamd_strtoul(param.value.data(), param.value.size(), &val);
			img->height = val;
		}

		if (param.type == html_component_type::RSPAMD_HTML_COMPONENT_WIDTH) {
			unsigned long val;

			rspamd_strtoul(param.value.data(), param.value.size(), &val);
			img->width = val;
		}

		/* TODO: rework to css at some time */
		if (param.type == html_component_type::RSPAMD_HTML_COMPONENT_STYLE) {
			if (img->height == 0) {
				auto style_st = param.value;
				auto pos = rspamd_substring_search_caseless(style_st.data(),
						style_st.size(),
						"height", sizeof("height") - 1);
				if (pos != -1) {
					auto substr = style_st.substr(pos + sizeof("height") - 1);

					for (auto i = 0; i < substr.size(); i++) {
						auto t = substr[i];
						if (g_ascii_isdigit (t)) {
							unsigned long val;
							rspamd_strtoul(substr.data(),
									substr.size(), &val);
							img->height = val;
							break;
						}
						else if (!g_ascii_isspace (t) && t != '=' && t != ':') {
							/* Fallback */
							break;
						}
					}
				}
			}
			if (img->width == 0) {
				auto style_st = param.value;
				auto pos = rspamd_substring_search_caseless(style_st.data(),
						style_st.size(),
						"width", sizeof("width") - 1);
				if (pos != -1) {
					auto substr = style_st.substr(pos + sizeof("width") - 1);

					for (auto i = 0; i < substr.size(); i++) {
						auto t = substr[i];
						if (g_ascii_isdigit (t)) {
							unsigned long val;
							rspamd_strtoul(substr.data(),
									substr.size(), &val);
							img->width = val;
							break;
						}
						else if (!g_ascii_isspace (t) && t != '=' && t != ':') {
							/* Fallback */
							break;
						}
					}
				}
			}
		}

		if (param.type == html_component_type::RSPAMD_HTML_COMPONENT_ALT) {
			if (!hc->parsed.empty() && !g_ascii_isspace (hc->parsed.back())) {
				/* Add a space */
				hc->parsed += ' ';
			}
			hc->parsed.append(param.value);

			if (!g_ascii_isspace (hc->parsed.back())) {
				/* Add a space */
				hc->parsed += ' ';
			}
		}
	}

	if (img->embedded_image) {
		if (img->height == 0) {
			img->height = img->embedded_image->height;
		}
		if (img->width == 0) {
			img->width = img->embedded_image->width;
		}
	}

	hc->images.push_back(img);
	tag->extra = img;
}

static auto
html_process_link_tag(rspamd_mempool_t *pool, struct html_tag *tag,
					  struct html_content *hc,
					  khash_t (rspamd_url_hash) *url_set,
					  GPtrArray *part_urls) -> void
{
	auto found_rel_maybe = tag->find_component(html_component_type::RSPAMD_HTML_COMPONENT_REL);

	if (found_rel_maybe) {
		if (found_rel_maybe.value() == "icon") {
			html_process_img_tag(pool, tag, hc, url_set, part_urls);
		}
	}
}

static auto
html_process_block_tag(rspamd_mempool_t *pool, struct html_tag *tag,
					   struct html_content *hc) -> void
{
	std::optional<css::css_value> maybe_fgcolor, maybe_bgcolor;

	for (const auto &param : tag->parameters) {
		if (param.type == html_component_type::RSPAMD_HTML_COMPONENT_COLOR) {
			maybe_fgcolor = css::css_value::maybe_color_from_string(param.value);
		}

		if (param.type == html_component_type::RSPAMD_HTML_COMPONENT_BGCOLOR) {
			maybe_bgcolor = css::css_value::maybe_color_from_string(param.value);
		}

		if (param.type == html_component_type::RSPAMD_HTML_COMPONENT_STYLE) {
			tag->block = rspamd::css::parse_css_declaration(pool, param.value);
		}
	}

	if (!tag->block) {
		tag->block = html_block::undefined_html_block_pool(pool);
	}

	if (maybe_fgcolor) {
		tag->block->set_fgcolor(maybe_fgcolor->to_color().value());
	}

	if (maybe_bgcolor) {
		tag->block->set_bgcolor(maybe_bgcolor->to_color().value());
	}
}

using tags_vector = std::vector<std::unique_ptr<struct html_tag>>;

static auto
tags_vector_ptr_dtor(void *ptr)
{
	auto *ptags = (tags_vector *)ptr;

	delete ptags;
}

static auto
html_process_input(rspamd_mempool_t *pool,
					GByteArray *in,
					GList **exceptions,
					khash_t (rspamd_url_hash) *url_set,
					GPtrArray *part_urls,
					bool allow_css) -> html_content *
{
	const gchar *p, *c, *end;
	guchar t;
	gboolean closing = FALSE, need_decode = FALSE, save_space = FALSE;
	guint obrace = 0, ebrace = 0;
	struct rspamd_url *url = NULL;
	gint len, href_offset = -1;
	struct html_tag *cur_tag = NULL, *content_tag = NULL;
	std::vector<html_tag *> tags_stack;
	struct tag_content_parser_state content_parser_env;

	enum {
		parse_start = 0,
		tag_begin,
		sgml_tag,
		xml_tag,
		compound_tag,
		comment_tag,
		comment_content,
		sgml_content,
		tag_content,
		tag_end,
		xml_tag_end,
		content_ignore,
		content_write,
		content_style,
		content_ignore_sp
	} state = parse_start;

	g_assert (in != NULL);
	g_assert (pool != NULL);

	struct html_content *hc = new html_content;
	rspamd_mempool_add_destructor(pool, html_content::html_content_dtor, hc);

	p = (const char *)in->data;
	c = p;
	end = p + in->len;

	while (p < end) {
		t = *p;

		switch (state) {
		case parse_start:
			if (t == '<') {
				state = tag_begin;
			}
			else {
				/* We have no starting tag, so assume that it's content */
				hc->flags |= RSPAMD_HTML_FLAG_BAD_START;
				state = content_write;
			}

			break;
		case tag_begin:
			switch (t) {
			case '<':
				p ++;
				closing = FALSE;
				break;
			case '!':
				state = sgml_tag;
				p ++;
				break;
			case '?':
				state = xml_tag;
				hc->flags |= RSPAMD_HTML_FLAG_XML;
				p ++;
				break;
			case '/':
				closing = TRUE;
				p ++;
				break;
			case '>':
				/* Empty tag */
				hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
				state = tag_end;
				continue;
			default:
				state = tag_content;
				content_parser_env.reset();

				hc->all_tags.emplace_back(std::make_unique<html_tag>());
				cur_tag = hc->all_tags.back().get();
				break;
			}

			break;

		case sgml_tag:
			switch (t) {
			case '[':
				state = compound_tag;
				obrace = 1;
				ebrace = 0;
				p ++;
				break;
			case '-':
				state = comment_tag;
				p ++;
				break;
			default:
				state = sgml_content;
				break;
			}

			break;

		case xml_tag:
			if (t == '?') {
				state = xml_tag_end;
			}
			else if (t == '>') {
				/* Misformed xml tag */
				hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
				state = tag_end;
				continue;
			}
			/* We efficiently ignore xml tags */
			p ++;
			break;

		case xml_tag_end:
			if (t == '>') {
				state = tag_end;
				continue;
			}
			else {
				hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
				p ++;
			}
			break;

		case compound_tag:
			if (t == '[') {
				obrace ++;
			}
			else if (t == ']') {
				ebrace ++;
			}
			else if (t == '>' && obrace == ebrace) {
				state = tag_end;
				continue;
			}
			p ++;
			break;

		case comment_tag:
			if (t != '-')  {
				hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
				state = tag_end;
			}
			else {
				p++;
				ebrace = 0;
				/*
				 * https://www.w3.org/TR/2012/WD-html5-20120329/syntax.html#syntax-comments
				 *  ... the text must not start with a single
				 *  U+003E GREATER-THAN SIGN character (>),
				 *  nor start with a "-" (U+002D) character followed by
				 *  a U+003E GREATER-THAN SIGN (>) character,
				 *  nor contain two consecutive U+002D HYPHEN-MINUS
				 *  characters (--), nor end with a "-" (U+002D) character.
				 */
				if (p[0] == '-' && p + 1 < end && p[1] == '>') {
					hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
					p ++;
					state = tag_end;
				}
				else if (*p == '>') {
					hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
					state = tag_end;
				}
				else {
					state = comment_content;
				}
			}
			break;

		case comment_content:
			if (t == '-') {
				ebrace ++;
			}
			else if (t == '>' && ebrace >= 2) {
				state = tag_end;
				continue;
			}
			else {
				ebrace = 0;
			}

			p ++;
			break;

		case content_ignore:
			if (t != '<') {
				p ++;
			}
			else {
				state = tag_begin;
			}
			break;

		case content_write:

			if (t != '<') {
				if (t == '&') {
					need_decode = TRUE;
				}
				else if (g_ascii_isspace (t)) {
					save_space = TRUE;

					if (p > c) {
						if (need_decode) {
							goffset old_offset = hc->parsed.size();

							if (content_tag) {
								if (content_tag->content_length == 0) {
									content_tag->content_offset = old_offset;
								}
							}

							hc->parsed.append(c, p - c);

							len = decode_html_entitles_inplace(
									hc->parsed.data() + old_offset,
									(std::size_t)(p - c));
							hc->parsed.resize(hc->parsed.size() + len - (p - c));

							if (content_tag) {
								content_tag->content_length += len;
							}
						}
						else {
							len = p - c;

							if (content_tag) {
								if (content_tag->content_length == 0) {
									content_tag->content_offset = hc->parsed.size();
								}

								content_tag->content_length += len;
							}

							hc->parsed.append(c, len);
						}
					}

					c = p;
					state = content_ignore_sp;
				}
				else {
					if (save_space) {
						/* Append one space if needed */
						if (!hc->parsed.empty() &&
							!g_ascii_isspace (hc->parsed.back())) {
							hc->parsed += " ";

							if (content_tag) {
								if (content_tag->content_length == 0) {
									/*
									 * Special case
									 * we have a space at the beginning but
									 * we have no set content_offset
									 * so we need to do it here
									 */
									content_tag->content_offset = hc->parsed.size();
								}
								else {
									content_tag->content_length++;
								}
							}
						}
						save_space = FALSE;
					}
				}
			}
			else {
				if (c != p) {

					if (need_decode) {
						goffset old_offset = hc->parsed.size();

						if (content_tag) {
							if (content_tag->content_length == 0) {
								content_tag->content_offset = hc->parsed.size();
							}
						}

						hc->parsed.append(c, p - c);
						len = decode_html_entitles_inplace(
								hc->parsed.data() + old_offset,
								(std::size_t)(p - c));
						hc->parsed.resize(hc->parsed.size() + len - (p - c));

						if (content_tag) {
							content_tag->content_length += len;
						}
					}
					else {
						len = p - c;

						if (content_tag) {
							if (content_tag->content_length == 0) {
								content_tag->content_offset = hc->parsed.size();
							}

							content_tag->content_length += len;
						}

						hc->parsed.append(c, len);
					}
				}

				content_tag = NULL;

				state = tag_begin;
				continue;
			}

			p ++;
			break;

		case content_style: {

			/*
			 * We just search for the first </s substring and then pass
			 * the content to the parser (if needed)
			 */
			auto end_style = rspamd_substring_search (p, end - p,
					"</", 2);
			if (end_style == -1 || g_ascii_tolower (p[end_style + 2]) != 's') {
				/* Invalid style */
				state = content_ignore;
			}
			else {

				if (allow_css) {
					auto ret_maybe =  rspamd::css::parse_css(pool, {p, std::size_t(end_style)},
							std::move(hc->css_style));

					if (!ret_maybe.has_value()) {
						auto err_str = fmt::format("cannot parse css (error code: {}): {}",
								static_cast<int>(ret_maybe.error().type),
								ret_maybe.error().description.value_or("unknown error"));
						msg_info_pool ("cannot parse css: %*s",
								(int)err_str.size(), err_str.data());
					}

					hc->css_style = ret_maybe.value();
				}

				p += end_style;
				state = tag_begin;
			}
			break;
		}

		case content_ignore_sp:
			if (!g_ascii_isspace (t)) {
				c = p;
				state = content_write;
				continue;
			}

			p ++;
			break;

		case sgml_content:
			/* TODO: parse DOCTYPE here */
			if (t == '>') {
				state = tag_end;
				/* We don't know a lot about sgml tags, ignore them */
				cur_tag = nullptr;
				continue;
			}
			p ++;
			break;

		case tag_content:
			html_parse_tag_content(pool, hc, cur_tag, p, content_parser_env);
			if (t == '>') {
				if (closing) {
					cur_tag->flags |= FL_CLOSING;

					if (cur_tag->flags & FL_CLOSED) {
						/* Bad mix of closed and closing */
						hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
					}

					closing = FALSE;
				}

				state = tag_end;
				continue;
			}
			p ++;
			break;

		case tag_end:
			content_parser_env.reset();

			if (cur_tag != nullptr) {

				if (html_process_tag(pool, hc, cur_tag, tags_stack)) {
					state = content_write;
					need_decode = FALSE;
				}
				else {
					if (cur_tag->id == Tag_STYLE) {
						state = content_style;
					}
					else {
						state = content_ignore;
					}
				}

				if (cur_tag->id != -1 && cur_tag->id < N_TAGS) {
					if (cur_tag->flags & CM_UNIQUE) {
						if (!hc->tags_seen[cur_tag->id]) {
							/* Duplicate tag has been found */
							hc->flags |= RSPAMD_HTML_FLAG_DUPLICATE_ELEMENTS;
						}
					}
					hc->tags_seen[cur_tag->id] = true;
				}

				if (!(cur_tag->flags & (FL_CLOSED|FL_CLOSING))) {
					content_tag = cur_tag;
				}

				/* Handle newlines */
				if (cur_tag->id == Tag_BR || cur_tag->id == Tag_HR) {
					if (!hc->parsed.empty() &&
						hc->parsed.back() != '\n') {

						hc->parsed += "\r\n";

						if (content_tag) {
							if (content_tag->content_length == 0) {
								/*
								 * Special case
								 * we have a \r\n at the beginning but
								 * we have no set content_offset
								 * so we need to do it here
								 */
								content_tag->content_offset = hc->parsed.size();
							}
							else {
								content_tag->content_length += 2;
							}
						}
					}
					save_space = FALSE;
				}

				if ((cur_tag->id == Tag_P ||
					 cur_tag->id == Tag_TR ||
					 cur_tag->id == Tag_DIV)) {
					if (!hc->parsed.empty() &&
						hc->parsed.back() != '\n') {

						hc->parsed += "\r\n";

						if (content_tag) {
							if (content_tag->content_length == 0) {
								/*
								 * Special case
								 * we have a \r\n at the beginning but
								 * we have no set content_offset
								 * so we need to get it here
								 */
								content_tag->content_offset = hc->parsed.size();
							}
							else {
								content_tag->content_length += 2;
							}
						}
					}
					save_space = FALSE;
				}

				/* XXX: uncomment when styles parsing is not so broken */
				if (cur_tag->flags & FL_HREF /* && !(cur_tag->flags & FL_IGNORE) */) {
					if (!(cur_tag->flags & (FL_CLOSING))) {
						auto maybe_url = html_process_url_tag(pool, cur_tag, hc);

						if (maybe_url) {
							url = maybe_url.value();

							if (url_set != NULL) {
								struct rspamd_url *maybe_existing =
										rspamd_url_set_add_or_return (url_set, maybe_url.value());
								if (maybe_existing == maybe_url.value()) {
									html_process_query_url(pool, url, url_set,
											part_urls);
								}
								else {
									url = maybe_existing;
									/* Increase count to avoid odd checks failure */
									url->count ++;
								}
							}

							href_offset = hc->parsed.size();
						}
					}

					if (cur_tag->id == Tag_A) {
						if (tags_stack.size() >= 2) {
							const auto *prev_tag = tags_stack.back()->parent;

							if (prev_tag && prev_tag->id == Tag_A &&
								!(prev_tag->flags & (FL_CLOSING)) &&
								std::holds_alternative<rspamd_url *>(prev_tag->extra)) {
								auto *prev_url = std::get<rspamd_url *>(prev_tag->extra);

								std::string_view disp_part{
										hc->parsed.data() + href_offset,
										hc->parsed.size() - href_offset};
								html_check_displayed_url (pool,
										exceptions, url_set,
										disp_part,
										href_offset,
										prev_url);
							}
						}

						if (cur_tag->flags & (FL_CLOSING)) {

							/* Insert exception */
							if (url != NULL && hc->parsed.size() > href_offset) {
								std::string_view disp_part{
										hc->parsed.data() + href_offset,
										hc->parsed.size() - href_offset};
								html_check_displayed_url (pool,
										exceptions, url_set,
										disp_part,
										href_offset,
										url);

							}

							href_offset = -1;
							url = NULL;
						}
					}
				}
				else if (cur_tag->id == Tag_BASE && !(cur_tag->flags & (FL_CLOSING))) {
					/*
					 * Base is allowed only within head tag but HTML is retarded
					 */
					if (hc->base_url == NULL) {
						auto maybe_url = html_process_url_tag(pool, cur_tag, hc);

						if (maybe_url) {
							msg_debug_html ("got valid base tag");
							hc->base_url = url;
							cur_tag->extra = url;
							cur_tag->flags |= FL_HREF;
						}
						else {
							msg_debug_html ("got invalid base tag!");
						}
					}
				}

				if (cur_tag->id == Tag_IMG && !(cur_tag->flags & FL_CLOSING)) {
					html_process_img_tag(pool, cur_tag, hc, url_set,
							part_urls);
				}
				else if (cur_tag->id == Tag_LINK && !(cur_tag->flags & FL_CLOSING)) {
					html_process_link_tag(pool, cur_tag, hc, url_set,
							part_urls);
				}

				if (cur_tag->flags & FL_BLOCK) {

					if (!(cur_tag->flags & FL_CLOSING)) {
						html_process_block_tag(pool, cur_tag, hc);
					}
				}
			}
			else {
				state = content_write;
			}


			p++;
			c = p;
			cur_tag = NULL;
			break;
		}
	}

	/* Summarize content length from children */
	hc->traverse_block_tags([](const html_tag *tag) -> bool {
		for (const auto *cld_tag : tag->children) {
			tag->content_length += cld_tag->content_length;
		}
		return true;
	}, html_content::traverse_type::POST_ORDER);

	/* Propagate styles */
	hc->traverse_block_tags([&hc, &exceptions,&pool](const html_tag *tag) -> bool {
		if (hc->css_style) {
			auto *css_block = hc->css_style->check_tag_block(tag);

			if (css_block) {
				if (tag->block) {
					tag->block->propagate_block(*css_block);
				}
				else {
					tag->block = css_block;
				}
			}
		}
		if (tag->block) {
			tag->block->compute_visibility();

			if (exceptions) {
				if (!tag->block->is_visible()) {
					if (tag->parent == nullptr || (tag->parent->block && tag->parent->block->is_visible())) {
						/* Add exception for an invisible element */
						auto * ex = rspamd_mempool_alloc_type (pool,struct rspamd_process_exception);
						ex->pos = tag->content_offset;
						ex->len = tag->content_length;
						ex->type = RSPAMD_EXCEPTION_INVISIBLE;
						ex->ptr = (void *)tag;

						*exceptions = g_list_prepend(*exceptions, ex);
					}
				}
				else if (*exceptions && tag->parent) {
					/* Current block is visible, check if parent is invisible */
					auto *ex = (struct rspamd_process_exception*)g_list_first(*exceptions)->data;

					/*
					 * TODO: we need to handle the following cases:
					 * <inv><vis><inv> -< insert one more exception
					 * <vis><inv> -< increase content_offset decrease length
					 * <inv><vis> -< decrease length
					 */
					if (ex && ex->type == RSPAMD_EXCEPTION_INVISIBLE &&
						ex->ptr == (void *)tag->parent) {
						auto *parent = tag->parent;

						if (tag->content_offset + tag->content_length ==
							parent->content_offset + parent->content_length) {
							/* <inv><vis> */
							ex->len -= tag->content_length;
						}
						else if (tag->content_offset == parent->content_offset) {
							/* <vis><inv> */
							ex->len -= tag->content_length;
							ex->pos += tag->content_length;
						}
						else if (tag->content_offset > ex->pos) {
							auto *nex = rspamd_mempool_alloc_type (pool,
									struct rspamd_process_exception);
							auto start_len = tag->content_offset - ex->pos;
							auto end_len = ex->len - tag->content_length - tag->content_length;
							nex->pos = tag->content_offset + tag->content_length;
							nex->len = end_len;
							nex->type = RSPAMD_EXCEPTION_INVISIBLE;
							nex->ptr = (void *)parent; /* ! */
							ex->len = start_len;
							*exceptions = g_list_prepend(*exceptions, ex);
						}

					}
				}
			}

			for (const auto *cld_tag : tag->children) {
				if (cld_tag->block) {
					cld_tag->block->propagate_block(*tag->block);
				}
				else {
					cld_tag->block = tag->block;
				}
			}
		}
		return true;
	}, html_content::traverse_type::PRE_ORDER);

	return hc;
}

static auto
html_find_image_by_cid(const html_content &hc, std::string_view cid)
	-> std::optional<const html_image *>
{
	for (const auto *html_image : hc.images) {
		/* Filter embedded images */
		if (html_image->flags & RSPAMD_HTML_FLAG_IMAGE_EMBEDDED &&
				html_image->src != nullptr) {
			if (cid == html_image->src) {
				return html_image;
			}
		}
	}

	return std::nullopt;
}

static auto
html_debug_structure(const html_content &hc) -> std::string
{
	std::string output;

	if (hc.root_tag) {
		auto rec_functor = [&](const html_tag *t, int level, auto rec_functor) -> void {
			std::string pluses(level, '+');
			output += fmt::format("{}{};", pluses, t->name);
			for (const auto *cld : t->children) {
				rec_functor(cld, level + 1, rec_functor);
			}
		};

		rec_functor(hc.root_tag, 1, rec_functor);
	}

	return output;
}

auto html_tag_by_name(const std::string_view &name)
	-> std::optional<tag_id_t>
{
	const auto *td = rspamd::html::html_tags_defs.by_name(name);

	if (td != nullptr) {
		return td->id;
	}

	return std::nullopt;
}

/*
 * Tests part
 */

TEST_SUITE("html") {
TEST_CASE("html parsing")
{

	const std::vector<std::pair<std::string, std::string>> cases{
			{"<html><!DOCTYPE html><body>",                    "+html;++body;"},
			{"<html><div><div></div></div></html>",            "+html;++div;+++div;"},
			{"<html><div><div></div></html>",                  "+html;++div;+++div;"},
			{"<html><div><div></div></html></div>",            "+html;++div;+++div;"},
			{"<p><p><a></p></a></a>",                          "+p;++p;+++a;"},
			{"<div><a href=\"http://example.com\"></div></a>", "+div;++a;"},
			{"<html><!DOCTYPE html><body><head><body></body></html></body></html>",
															   "+html;++body;+++head;++++body;"}
	};

	rspamd_url_init(NULL);
	auto *pool = rspamd_mempool_new(rspamd_mempool_suggest_size(),
			"html", 0);

	for (const auto &c : cases) {
		GByteArray *tmp = g_byte_array_sized_new(c.first.size());
		g_byte_array_append(tmp, (const guint8 *) c.first.data(), c.first.size());
		auto *hc = html_process_input(pool, tmp, nullptr, nullptr, nullptr, true);
		CHECK(hc != nullptr);
		auto dump = html_debug_structure(*hc);
		CHECK(c.second == dump);
		g_byte_array_free(tmp, TRUE);
	}

	rspamd_mempool_delete(pool);
}
}

}

void *
rspamd_html_process_part_full(rspamd_mempool_t *pool,
							  GByteArray *in, GList **exceptions,
							  khash_t (rspamd_url_hash) *url_set,
							  GPtrArray *part_urls,
							  bool allow_css)
{
	return rspamd::html::html_process_input(pool, in, exceptions, url_set,
			part_urls, allow_css);
}

void *
rspamd_html_process_part(rspamd_mempool_t *pool,
						 GByteArray *in)
{
	return rspamd_html_process_part_full (pool, in, NULL,
			NULL, NULL, FALSE);
}

guint
rspamd_html_decode_entitles_inplace (gchar *s, gsize len)
{
	return rspamd::html::decode_html_entitles_inplace(s, len);
}

gint
rspamd_html_tag_by_name(const gchar *name)
{
	const auto *td = rspamd::html::html_tags_defs.by_name(name);

	if (td != nullptr) {
		return td->id;
	}

	return -1;
}

gboolean
rspamd_html_tag_seen(void *ptr, const gchar *tagname)
{
	gint id;
	auto *hc = rspamd::html::html_content::from_ptr(ptr);

	g_assert (hc != NULL);

	id = rspamd_html_tag_by_name(tagname);

	if (id != -1) {
		return hc->tags_seen[id];
	}

	return FALSE;
}

const gchar *
rspamd_html_tag_by_id(gint id)
{
	const auto *td = rspamd::html::html_tags_defs.by_id(id);

	if (td != nullptr) {
		return td->name.c_str();
	}

	return nullptr;
}

const gchar *
rspamd_html_tag_name(void *p, gsize *len)
{
	auto *tag = reinterpret_cast<rspamd::html::html_tag *>(p);

	if (len) {
		*len = tag->name.size();
	}

	return tag->name.data();
}

struct html_image*
rspamd_html_find_embedded_image(void *html_content,
								const char *cid, gsize cid_len)
{
	auto *hc = rspamd::html::html_content::from_ptr(html_content);

	auto maybe_img = rspamd::html::html_find_image_by_cid(*hc, {cid, cid_len});

	if (maybe_img) {
		return (html_image *)maybe_img.value();
	}

	return nullptr;
}

bool
rspamd_html_get_parsed_content(void *html_content, rspamd_ftok_t *dest)
{
	auto *hc = rspamd::html::html_content::from_ptr(html_content);

	dest->begin = hc->parsed.data();
	dest->len = hc->parsed.size();

	return true;
}