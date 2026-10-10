/*
===========================================================================
Copyright (C) 2026 Project: Omaha

This file is part of Project: Omaha source code.

Project: Omaha builds upon OpenMoHAA / ioquake3 / F.A.K.K. foundations.
Project: Omaha source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Project: Omaha source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Project: Omaha source code; if not, see COPYING.txt in the
source tree, or write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "practice_scenario.hpp"
#include "json.hpp"

#include <cctype>
#include <cmath>
#include <cstring>

static bool Practice_ReadVec3(const nlohmann::json &j, vec3_t out)
{
	if (!j.is_array() || j.size() < 3) {
		return false;
	}
	out[0] = j.at(0).get<float>();
	out[1] = j.at(1).get<float>();
	out[2] = j.at(2).get<float>();
	if (!std::isfinite(out[0]) || !std::isfinite(out[1]) || !std::isfinite(out[2])) {
		return false;
	}
	if (fabsf(out[0]) > 1.0e6f || fabsf(out[1]) > 1.0e6f || fabsf(out[2]) > 1.0e6f) {
		return false;
	}
	return true;
}

static nlohmann::json Practice_WriteVec3(const vec3_t v)
{
	return nlohmann::json::array({v[0], v[1], v[2]});
}

static bool Practice_SegmentSafe(const char *seg, size_t len)
{
	size_t i;

	if (!seg || len == 0) {
		return false;
	}
	for (i = 0; i < len; i++) {
		const unsigned char c = (unsigned char)seg[i];

		if (c < 0x20 || c == 0x7f) {
			return false;
		}
		if (c == '"' || c == ';' || c == '\\' || c == '$' || c == '%' || c == '\'' || c == ' ') {
			return false;
		}
		if (c != '/' && c != '-' && c != '_' && c != '.' && !isalnum(c)) {
			return false;
		}
	}
	return true;
}

bool Practice_IsSafeToken(const char *s)
{
	size_t i;

	if (!s || !s[0]) {
		return false;
	}
	for (i = 0; s[i]; i++) {
		const unsigned char c = (unsigned char)s[i];

		if (c < 0x20 || c == 0x7f) {
			return false;
		}
		if (c == '"' || c == ';' || c == '\\' || c == '$' || c == '%' || c == '\n' || c == '\r') {
			return false;
		}
	}
	return true;
}

bool Practice_IsSafeDifficulty(const char *s)
{
	if (!s) {
		return false;
	}
	return !Q_stricmp(s, "easy") || !Q_stricmp(s, "medium") || !Q_stricmp(s, "hard");
}

bool Practice_IsSafeCategory(const char *s)
{
	return Practice_CategoryFromName(s) == PRACTICE_CAT_ANGLES
		|| Practice_CategoryFromName(s) == PRACTICE_CAT_NADES;
}

bool Practice_IsPracticeScenarioPath(const char *qpath)
{
	const char *p;
	const char *seg;
	size_t      len;

	if (!qpath || Q_stricmpn(qpath, "practice/", 9) != 0) {
		return false;
	}
	len = strlen(qpath);
	if (len < 14 || Q_stricmp(qpath + len - 5, ".json") != 0) {
		return false;
	}
	if (strstr(qpath, "..") || strstr(qpath, "::") || strstr(qpath, "//")) {
		return false;
	}
	p   = qpath + 9;
	seg = p;
	while (*p) {
		if (*p == '/') {
			if (!Practice_SegmentSafe(seg, (size_t)(p - seg))) {
				return false;
			}
			seg = p + 1;
		}
		p++;
	}
	// Final segment is name.json — allow the dot before json only as extension.
	if (seg >= qpath + len - 5) {
		return false;
	}
	if (!Practice_SegmentSafe(seg, (size_t)((qpath + len - 5) - seg))) {
		return false;
	}
	return Practice_IsSafeToken(qpath);
}

bool Practice_IsWritableScenarioPath(const char *qpath)
{
	return Practice_IsPracticeScenarioPath(qpath);
}

void Practice_SanitizeMetaField(const char *in, char *out, size_t outSize)
{
	size_t n = 0;

	if (!out || outSize == 0) {
		return;
	}
	out[0] = 0;
	if (!in) {
		return;
	}
	if (outSize > PRACTICE_META_MAX_CHARS + 1) {
		outSize = PRACTICE_META_MAX_CHARS + 1;
	}
	for (; *in && n + 1 < outSize; in++) {
		unsigned char c = (unsigned char)*in;

		if (c < ' ' || c == 127 || c == '%') {
			continue;
		}
		if (c == '"') {
			c = '\'';
		}
		out[n++] = (char)c;
	}
	out[n] = 0;
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) {
		out[--n] = 0;
	}
}

void PracticeScenario_MakePath(const char *map, const char *name, char *out, size_t outSize)
{
	char   mapPath[MAX_QPATH];
	char   namePath[MAX_QPATH];
	size_t i;

	if (!out || outSize == 0) {
		return;
	}
	out[0] = 0;
	if (!map || !map[0] || !name || !name[0]) {
		return;
	}

	Q_strncpyz(mapPath, map, sizeof(mapPath));
	Q_strncpyz(namePath, name, sizeof(namePath));
	for (i = 0; mapPath[i]; i++) {
		if (mapPath[i] == '\\') {
			mapPath[i] = '/';
		} else if (mapPath[i] != '/' && mapPath[i] != '-' && mapPath[i] != '_'
				   && !isalnum((unsigned char)mapPath[i])) {
			mapPath[i] = '_';
		}
	}
	for (i = 0; namePath[i]; i++) {
		if (!isalnum((unsigned char)namePath[i]) && namePath[i] != '_' && namePath[i] != '-') {
			namePath[i] = '_';
		}
	}
	Com_sprintf(out, outSize, "practice/%s/%s.json", mapPath, namePath);
}

bool PracticeScenario_Parse(const char *text, PracticeScenario *out, char *err, size_t errSize)
{
	if (!out) {
		return false;
	}
	out->Clear();
	if (!text || !text[0]) {
		if (err && errSize) {
			Q_strncpyz(err, "empty practice file", errSize);
		}
		return false;
	}

	try {
		nlohmann::json j = nlohmann::json::parse(text);
		out->version     = j.value("version", 1);
		Q_strncpyz(out->map, j.value("map", std::string()).c_str(), sizeof(out->map));
		Q_strncpyz(out->name, j.value("name", std::string()).c_str(), sizeof(out->name));
		Q_strncpyz(out->author, j.value("author", std::string()).c_str(), sizeof(out->author));
		out->category = Practice_CategoryFromName(j.value("category", std::string()).c_str());
		out->maxAlive = j.value("maxAlive", 32);
		if (out->maxAlive < 1) {
			out->maxAlive = 1;
		}
		if (out->maxAlive > 32) {
			out->maxAlive = 32;
		}
		out->bestTimeMs = j.value("bestTimeMs", 0);
		if (out->bestTimeMs < 0) {
			out->bestTimeMs = 0;
		}
		{
			const std::string bd = j.value("bestDifficulty", std::string());

			if (!Q_stricmp(bd.c_str(), "hard")) {
				Q_strncpyz(out->bestDifficulty, "hard", sizeof(out->bestDifficulty));
			} else if (!Q_stricmp(bd.c_str(), "easy")) {
				Q_strncpyz(out->bestDifficulty, "easy", sizeof(out->bestDifficulty));
			} else if (!Q_stricmp(bd.c_str(), "medium")) {
				Q_strncpyz(out->bestDifficulty, "medium", sizeof(out->bestDifficulty));
			} else {
				out->bestDifficulty[0] = 0;
			}
		}

		bool missingIds = false;

		if (j.contains("nodes") && j["nodes"].is_array()) {
			if ((int)j["nodes"].size() > PRACTICE_MAX_NODES) {
				if (err && errSize) {
					Q_strncpyz(err, "too many nodes", errSize);
				}
				out->Clear();
				return false;
			}
			for (const auto &n : j["nodes"]) {
				PracticeNode node;
				if (n.contains("id") && n["id"].is_number_integer()) {
					node.id = n["id"].get<int>();
				} else {
					node.id    = -1;
					missingIds = true;
				}
				node.type = Practice_NodeTypeFromName(n.value("type", std::string("spot")).c_str());
				if (n.contains("origin") && !Practice_ReadVec3(n["origin"], node.origin.v)) {
					if (err && errSize) {
						Q_strncpyz(err, "invalid node origin", errSize);
					}
					out->Clear();
					return false;
				}
				if (n.contains("angles") && !Practice_ReadVec3(n["angles"], node.angles.v)) {
					if (err && errSize) {
						Q_strncpyz(err, "invalid node angles", errSize);
					}
					out->Clear();
					return false;
				}
				node.stance = Practice_StanceFromName(n.value("stance", std::string("stand")).c_str());
				node.lean   = Practice_LeanFromName(n.value("lean", std::string("none")).c_str());
				out->nodes.push_back(node);
			}
		}

		if (j.contains("throws") && j["throws"].is_array()) {
			if ((int)j["throws"].size() > PRACTICE_MAX_THROWS) {
				if (err && errSize) {
					Q_strncpyz(err, "too many throws", errSize);
				}
				out->Clear();
				return false;
			}
			for (const auto &t : j["throws"]) {
				PracticeThrow th;
				if (t.contains("stand") && !Practice_ReadVec3(t["stand"], th.stand.v)) {
					if (err && errSize) {
						Q_strncpyz(err, "invalid throw stand", errSize);
					}
					out->Clear();
					return false;
				}
				if (t.contains("aim") && !Practice_ReadVec3(t["aim"], th.aim.v)) {
					if (err && errSize) {
						Q_strncpyz(err, "invalid throw aim", errSize);
					}
					out->Clear();
					return false;
				}
				if (t.contains("eye")) {
					if (!Practice_ReadVec3(t["eye"], th.eye.v)) {
						if (err && errSize) {
							Q_strncpyz(err, "invalid throw eye", errSize);
						}
						out->Clear();
						return false;
					}
					th.hasEye = true;
				}
				if (t.contains("target") && !Practice_ReadVec3(t["target"], th.target.v)) {
					if (err && errSize) {
						Q_strncpyz(err, "invalid throw target", errSize);
					}
					out->Clear();
					return false;
				}
				th.radius = t.value("radius", 96.0f);
				th.charge = t.value("charge", 1.0f);
				if (!std::isfinite(th.radius)) {
					th.radius = 96.0f;
				}
				if (!std::isfinite(th.charge)) {
					th.charge = 1.0f;
				}
				if (th.radius < 16.0f) {
					th.radius = 16.0f;
				} else if (th.radius > 512.0f) {
					th.radius = 512.0f;
				}
				if (th.charge < 0.0f) {
					th.charge = 0.0f;
				} else if (th.charge > 1.0f) {
					th.charge = 1.0f;
				}
				th.stance = Practice_StanceFromName(t.value("stance", std::string("stand")).c_str());
				th.lean   = Practice_LeanFromName(t.value("lean", std::string("none")).c_str());
				th.dummy  = t.value("dummy", true);
				out->throwsList.push_back(th);
			}
		}

		out->nodesRenumbered = out->RenumberNodes() || missingIds;
		out->RebuildDerived();
	} catch (const std::exception &ex) {
		if (err && errSize) {
			Q_strncpyz(err, ex.what(), errSize);
		}
		out->Clear();
		return false;
	}

	return true;
}

bool PracticeScenario_Dump(const PracticeScenario &in, std::string *out)
{
	if (!out) {
		return false;
	}

	nlohmann::json j;
	j["version"]    = 2;
	j["category"]   = Practice_CategoryName(in.category);
	j["map"]        = in.map;
	j["name"]       = in.name;
	j["author"]     = in.author;
	j["maxAlive"]   = in.maxAlive;
	j["bestTimeMs"] = in.bestTimeMs;
	if (in.bestDifficulty[0]) {
		j["bestDifficulty"] = in.bestDifficulty;
	}

	nlohmann::json nodes = nlohmann::json::array();
	for (const PracticeNode &node : in.nodes) {
		nlohmann::json n;
		n["id"]     = node.id;
		n["type"]   = Practice_NodeTypeName(node.type);
		n["origin"] = Practice_WriteVec3(node.origin.v);
		if (node.type == PRACTICE_NODE_START || node.type == PRACTICE_NODE_SPOT) {
			n["angles"] = Practice_WriteVec3(node.angles.v);
		}
		if (node.type == PRACTICE_NODE_SPOT) {
			n["stance"] = Practice_StanceName(node.stance);
			n["lean"]   = Practice_LeanName(node.lean);
		}
		nodes.push_back(n);
	}
	j["nodes"] = nodes;

	nlohmann::json throws = nlohmann::json::array();
	for (const PracticeThrow &th : in.throwsList) {
		nlohmann::json t;
		t["stand"] = Practice_WriteVec3(th.stand.v);
		t["aim"]   = Practice_WriteVec3(th.aim.v);
		if (th.hasEye) {
			t["eye"] = Practice_WriteVec3(th.eye.v);
		}
		t["target"] = Practice_WriteVec3(th.target.v);
		t["radius"] = th.radius;
		t["charge"] = th.charge;
		t["stance"] = Practice_StanceName(th.stance);
		t["lean"]   = Practice_LeanName(th.lean);
		t["dummy"]  = th.dummy;
		throws.push_back(t);
	}
	j["throws"] = throws;

	*out = j.dump(2);
	*out += '\n';
	return true;
}
