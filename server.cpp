#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <map>
#include <unordered_set>
#include <climits>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <fstream>
#include <thread>
#include <cstdlib>
#include <cctype>
#include <random>

// POSIX Headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

// SQLite Header: use bundled header if present, otherwise system/Homebrew SQLite
#if __has_include("include/sqlite3.h")
#include "include/sqlite3.h"
#else
#include <sqlite3.h>
#endif

using namespace std;

// --- DATA MODELS ---
struct Emergency {
    int id;
    string patientName;
    int severity;
    string requiredSpecialty;
    string location;
    long long timestamp;

    bool operator>(const Emergency& other) const {
        if (severity == other.severity)
            return timestamp > other.timestamp; // earlier arrival wins ties
        return severity < other.severity; // higher severity (5) gets higher priority
    }
};

struct Ambulance {
    string id;
    string location;
    bool isAvailable;
    string type;
};

struct Hospital {
    string id;
    string name;
    string location;
    unordered_set<string> specialties;
    int availableBeds;
};

struct Edge {
    string toNode;
    int travelTimeMinutes;
};

// --- DISPATCH ENGINE & DBMS INTEGRATION ---
class DispatchEngine {
public:
    priority_queue<Emergency, vector<Emergency>, greater<Emergency>> emergencyQueue;
    map<string, Ambulance> ambulances;
    map<string, Hospital> hospitals;
    map<string, vector<Edge>> cityGraph;

    mutex queueMutex;
    mutex ambulanceMutex;
    mutex hospitalMutex;
    mutex dbMutex;

    sqlite3* db = nullptr;
    int emergencyCounter = 1;

    DispatchEngine() {
        const char* configuredDbPath = std::getenv("MEDEVAC_DB_PATH");
        const char* dbPath = (configuredDbPath && *configuredDbPath) ? configuredDbPath : "dispatch_system.db";
        if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
            cerr << "Error opening SQLite DB: " << sqlite3_errmsg(db) << "\n";
        } else {
            cout << "Successfully connected to SQLite Database!\n";
            initDatabaseTables();
            // Do not erase live records on every server restart.
            loadDataFromDatabase();
        }
    }

    ~DispatchEngine() {
        if (db) sqlite3_close(db);
    }

    void autoResetOnStartup() {
        lock_guard<mutex> lock(dbMutex);
        
        // Reset SQLite database state
        sqlite3_exec(db, "UPDATE ambulances SET is_available = 1;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "UPDATE hospitals SET available_beds = 5;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DELETE FROM emergency_logs;", nullptr, nullptr, nullptr);

        // Clear in-memory queue and sync emergency ID counter
        {
            lock_guard<mutex> qLock(queueMutex);
            while (!emergencyQueue.empty()) emergencyQueue.pop();
            emergencyCounter = 1;
        }
    }

    void initDatabaseTables() {
        lock_guard<mutex> lock(dbMutex);

        const char* schemaSql = R"(
            CREATE TABLE IF NOT EXISTS roads (
                source TEXT,
                target TEXT,
                travel_time_mins INTEGER
            );

            CREATE TABLE IF NOT EXISTS ambulances (
                id TEXT PRIMARY KEY,
                location TEXT,
                is_available INTEGER,
                type TEXT
            );

            CREATE TABLE IF NOT EXISTS hospitals (
                id TEXT PRIMARY KEY,
                name TEXT,
                location TEXT,
                specialties TEXT,
                available_beds INTEGER
            );

            CREATE TABLE IF NOT EXISTS emergency_logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                patient_name TEXT,
                severity INTEGER,
                specialty TEXT,
                location TEXT,
                assigned_ambulance TEXT,
                assigned_hospital TEXT
            );

            CREATE TABLE IF NOT EXISTS organization_registrations (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                role TEXT NOT NULL,
                organization_name TEXT NOT NULL,
                location TEXT NOT NULL,
                specialties TEXT DEFAULT '',
                available_beds INTEGER DEFAULT 0,
                ambulance_type TEXT DEFAULT 'BLS',
                status TEXT NOT NULL DEFAULT 'PENDING',
                api_key TEXT DEFAULT '',
                resource_id TEXT DEFAULT '',
                created_at TEXT DEFAULT CURRENT_TIMESTAMP
            );
        )";

        sqlite3_exec(db, schemaSql, nullptr, nullptr, nullptr);

        sqlite3_stmt* checkStmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM roads;", -1, &checkStmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(checkStmt) == SQLITE_ROW && sqlite3_column_int(checkStmt, 0) == 0) {
                cout << "Seeding High-Density Street Waypoints into SQLite...\n";
                const char* seedSql = R"(
                    INSERT INTO roads VALUES 
                        ('Kothrud_Stand', 'Karve_Road_Curve_1', 1),
                        ('Karve_Road_Curve_1', 'Paud_Phata_Flyover', 1),
                        ('Paud_Phata_Flyover', 'Sahyadri_Hospital_Node', 1),
                        ('Sahyadri_Hospital_Node', 'Nal_Stop_Chowk', 1),
                        ('Nal_Stop_Chowk', 'Garware_College_Bend', 1),
                        ('Garware_College_Bend', 'Deccan_Goodluck_Chowk', 1),
                        ('Deccan_Goodluck_Chowk', 'FC_Road_Entry', 1),
                        ('FC_Road_Entry', 'Fergusson_College_Gate', 2),
                        ('Fergusson_College_Gate', 'Sanjeevan_Hospital_Node', 1),
                        ('Sanjeevan_Hospital_Node', 'Shivajinagar_Station', 2),
                        ('Deccan_Goodluck_Chowk', 'Sambhaji_Bridge_Entry', 1),
                        ('Sambhaji_Bridge_Entry', 'Poona_Hospital_Node', 1),
                        ('Poona_Hospital_Node', 'Alka_Talkies_Chowk', 1),
                        ('Alka_Talkies_Chowk', 'Tilak_Road_Bend', 2),
                        ('Tilak_Road_Bend', 'Swargate_Chowk', 2),
                        ('Swargate_Chowk', 'Seven_Loves_Chowk', 2),
                        ('Seven_Loves_Chowk', 'Camp_MG_Road', 3);

                    INSERT INTO ambulances VALUES
                        ('Amb_Kothrud_1', 'Kothrud_Stand', 1, 'ALS'),
                        ('Amb_Swargate_1', 'Swargate_Chowk', 1, 'BLS'),
                        ('Amb_Camp_1', 'Camp_MG_Road', 1, 'ALS');

                    INSERT INTO hospitals VALUES
                        ('Hosp_Sahyadri', 'Sahyadri Specialty Hospital', 'Sahyadri_Hospital_Node', 'Cardiac,Trauma,General', 5),
                        ('Hosp_Poona', 'Poona Hospital & Research Centre', 'Poona_Hospital_Node', 'General,Trauma', 3),
                        ('Hosp_Sanjeevan', 'Sanjeevan Cardiac Center', 'Sanjeevan_Hospital_Node', 'Cardiac,General', 2);
                )";
                sqlite3_exec(db, seedSql, nullptr, nullptr, nullptr);
            }
        }
        if (checkStmt) sqlite3_finalize(checkStmt);
    }

    void loadDataFromDatabase() {
        lock_guard<mutex> lock(dbMutex);
        sqlite3_stmt* stmt = nullptr;

        cityGraph.clear();
        ambulances.clear();
        hospitals.clear();

        const char* roadQuery = "SELECT source, target, travel_time_mins FROM roads;";
        if (sqlite3_prepare_v2(db, roadQuery, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* tgt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (!src || !tgt) continue;

                int time = sqlite3_column_int(stmt, 2);
                cityGraph[src].push_back({tgt, time});
                cityGraph[tgt].push_back({src, time});
            }
        }
        if (stmt) sqlite3_finalize(stmt);

        const char* ambQuery = "SELECT id, location, is_available, type FROM ambulances;";
        if (sqlite3_prepare_v2(db, ambQuery, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* loc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                if (!id || !loc || !type) continue;

                ambulances[id] = {id, loc, sqlite3_column_int(stmt, 2) == 1, type};
            }
        }
        if (stmt) sqlite3_finalize(stmt);

        const char* hospQuery = "SELECT id, name, location, specialties, available_beds FROM hospitals;";
        if (sqlite3_prepare_v2(db, hospQuery, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                const char* loc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                const char* specsStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                if (!id || !name || !loc || !specsStr) continue;

                unordered_set<string> specs;
                stringstream ss(specsStr);
                string item;
                while (getline(ss, item, ',')) {
                    size_t start = item.find_first_not_of(" \t");
                    size_t end = item.find_last_not_of(" \t");
                    if (start != string::npos)
                        specs.insert(item.substr(start, end - start + 1));
                }

                hospitals[id] = {id, name, loc, std::move(specs), sqlite3_column_int(stmt, 4)};
            }
        }
        if (stmt) sqlite3_finalize(stmt);

        cout << "Loaded " << cityGraph.size() << " street waypoints/nodes, "
             << ambulances.size() << " ambulances, "
             << hospitals.size() << " hospitals.\n";
    }

    void logDispatchToDatabase(const Emergency& e, const string& ambId, const string& hospId) {
        lock_guard<mutex> lock(dbMutex);

        string updateAmb = "UPDATE ambulances SET is_available = 0 WHERE id = '" + ambId + "';";
        sqlite3_exec(db, updateAmb.c_str(), nullptr, nullptr, nullptr);

        string updateHosp = "UPDATE hospitals SET available_beds = available_beds - 1 WHERE id = '" + hospId + "';";
        sqlite3_exec(db, updateHosp.c_str(), nullptr, nullptr, nullptr);

        string insertLog = "INSERT INTO emergency_logs (patient_name, severity, specialty, location, assigned_ambulance, assigned_hospital) VALUES ('" +
                           e.patientName + "', " + to_string(e.severity) + ", '" + e.requiredSpecialty + "', '" +
                           e.location + "', '" + ambId + "', '" + hospId + "');";
        sqlite3_exec(db, insertLog.c_str(), nullptr, nullptr, nullptr);
    }

    bool resolveEmergency(int emergencyId) {
        string ambId, hospId;
        {
            lock_guard<mutex> lock(dbMutex);
            string selectQuery = "SELECT assigned_ambulance, assigned_hospital FROM emergency_logs WHERE id = " + to_string(emergencyId) + ";";
            sqlite3_stmt* stmt = nullptr;

            if (sqlite3_prepare_v2(db, selectQuery.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* ambText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    const char* hospText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    if (ambText) ambId = ambText;
                    if (hospText) hospId = hospText;
                }
            }
            if (stmt) sqlite3_finalize(stmt);

            if (ambId.empty() || hospId.empty()) return false;

            string freeAmbSql = "UPDATE ambulances SET is_available = 1 WHERE id = '" + ambId + "';";
            string freeHospSql = "UPDATE hospitals SET available_beds = available_beds + 1 WHERE id = '" + hospId + "';";
            string updateLogSql = "UPDATE emergency_logs SET assigned_ambulance = 'RESOLVED (' || assigned_ambulance || ')' WHERE id = " + to_string(emergencyId) + ";";

            sqlite3_exec(db, freeAmbSql.c_str(), nullptr, nullptr, nullptr);
            sqlite3_exec(db, freeHospSql.c_str(), nullptr, nullptr, nullptr);
            sqlite3_exec(db, updateLogSql.c_str(), nullptr, nullptr, nullptr);
        }

        {
            lock_guard<mutex> lock(ambulanceMutex);
            auto it = ambulances.find(ambId);
            if (it != ambulances.end()) it->second.isAvailable = true;
        }

        {
            lock_guard<mutex> lock(hospitalMutex);
            auto it = hospitals.find(hospId);
            if (it != hospitals.end()) it->second.availableBeds++;
        }

        return true;
    }

    pair<int, vector<string>> dijkstra(const string& startNode, const string& targetNode) {
        if (startNode == targetNode) return {0, {startNode}};

        map<string, int> dist;
        map<string, string> parent;

        for (const auto& [node, _] : cityGraph) {
            dist[node] = INT_MAX;
        }
        dist[startNode] = INT_MAX;
        dist[targetNode] = INT_MAX;

        priority_queue<pair<int, string>, vector<pair<int, string>>, greater<pair<int, string>>> pq;

        dist[startNode] = 0;
        pq.push({0, startNode});

        while (!pq.empty()) {
            auto [d, u] = pq.top();
            pq.pop();

            if (d > dist[u]) continue;
            if (u == targetNode) break;

            auto it = cityGraph.find(u);
            if (it == cityGraph.end()) continue;

            for (const auto& edge : it->second) {
                if (dist[u] != INT_MAX && dist[u] + edge.travelTimeMinutes < dist[edge.toNode]) {
                    dist[edge.toNode] = dist[u] + edge.travelTimeMinutes;
                    parent[edge.toNode] = u;
                    pq.push({dist[edge.toNode], edge.toNode});
                }
            }
        }

        if (dist[targetNode] == INT_MAX) return {INT_MAX, {}};

        vector<string> path;
        string curr = targetNode;
        while (curr != startNode) {
            path.push_back(curr);
            auto it = parent.find(curr);
            if (it == parent.end()) return {INT_MAX, {}};
            curr = it->second;
        }
        path.push_back(startNode);
        reverse(path.begin(), path.end());

        return {dist[targetNode], path};
    }
};

DispatchEngine engine;

// --- JSON UTILITIES ---
string escapeJson(const string& s) {
    string result = "\"";
    result.reserve(s.size() + 10);
    for (char c : s) {
        switch (c) {
            case '\"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += c;
        }
    }
    result += "\"";
    return result;
}

string getJsonVal(const string& body, const string& key) {
    size_t pos = body.find("\"" + key + "\"");
    if (pos == string::npos) return "";
    size_t colon = body.find(':', pos);
    if (colon == string::npos) return "";
    size_t start = colon + 1;
    while (start < body.size() && isspace(static_cast<unsigned char>(body[start]))) ++start;
    if (start >= body.size()) return "";

    if (body[start] == '"') {
        ++start;
        string value;
        bool escaped = false;
        for (size_t i = start; i < body.size(); ++i) {
            char c = body[i];
            if (escaped) {
                switch (c) {
                    case 'n': value += '\n'; break;
                    case 'r': value += '\r'; break;
                    case 't': value += '\t'; break;
                    default: value += c; break;
                }
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                return value;
            } else {
                value += c;
            }
        }
        return value;
    }

    size_t end = start;
    while (end < body.size() && body[end] != ',' && body[end] != '}' &&
           !isspace(static_cast<unsigned char>(body[end]))) ++end;
    return body.substr(start, end - start);
}

string vectorToJson(const vector<string>& vec) {
    string result = "[";
    for (size_t i = 0; i < vec.size(); ++i) {
        result += escapeJson(vec[i]);
        if (i + 1 < vec.size()) result += ",";
    }
    result += "]";
    return result;
}

string getHeaderVal(const string& request, const string& headerName) {
    // HTTP header names are case-insensitive.
    string wanted = headerName;
    transform(wanted.begin(), wanted.end(), wanted.begin(),
              [](unsigned char c) { return static_cast<char>(tolower(c)); });

    size_t lineStart = request.find("\r\n");
    if (lineStart == string::npos) return "";
    lineStart += 2;

    while (lineStart < request.size()) {
        size_t lineEnd = request.find("\r\n", lineStart);
        if (lineEnd == string::npos) lineEnd = request.size();
        if (lineEnd == lineStart) break;

        string line = request.substr(lineStart, lineEnd - lineStart);
        size_t colon = line.find(':');
        if (colon != string::npos) {
            string name = line.substr(0, colon);
            transform(name.begin(), name.end(), name.begin(),
                      [](unsigned char c) { return static_cast<char>(tolower(c)); });
            if (name == wanted) {
                size_t first = line.find_first_not_of(" \t", colon + 1);
                if (first == string::npos) return "";
                size_t last = line.find_last_not_of(" \t");
                return line.substr(first, last - first + 1);
            }
        }
        if (lineEnd >= request.size()) break;
        lineStart = lineEnd + 2;
    }
    return "";
}

string sqlQuote(const string& value) {
    char* escaped = sqlite3_mprintf("%q", value.c_str());
    string result = escaped ? escaped : "";
    if (escaped) sqlite3_free(escaped);
    return result;
}

string adminSecret() {
    const char* value = std::getenv("MEDEVAC_ADMIN_KEY");
    return (value && *value) ? string(value) : string("medevac-demo-admin-2026");
}

string newApiKey() {
    static random_device rd;
    static mt19937_64 gen(rd());
    static const char* hex = "0123456789abcdef";
    string out = "mdv_";
    for (int i = 0; i < 48; ++i) out += hex[gen() % 16];
    return out;
}

string makeHttpResponse(const string& status, const string& contentType, const string& body) {
    return "HTTP/1.1 " + status + "\r\n"
           "Content-Type: " + contentType + "\r\n"
           "Content-Length: " + to_string(body.size()) + "\r\n"
           "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Headers: Content-Type, Authorization, X-Admin-Key\r\n"
           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           "Connection: close\r\n\r\n" + body;
}

// --- CLIENT HANDLER ---
void handleClient(int clientSocket) {
    char buffer[8192] = {0};
    ssize_t bytesRead = read(clientSocket, buffer, sizeof(buffer) - 1);

    if (bytesRead <= 0) {
        close(clientSocket);
        return;
    }

    buffer[bytesRead] = '\0';
    string request(buffer);
    string response;

    if (request.rfind("OPTIONS ", 0) == 0) {
        response = makeHttpResponse("204 No Content", "text/plain", "");
    }
    // Public registration: role must be hospital or ambulance_provider.
    else if (request.find("POST /api/register") != string::npos) {
        size_t bodyPos = request.find("\r\n\r\n");
        string body = bodyPos == string::npos ? "" : request.substr(bodyPos + 4);
        string role = getJsonVal(body, "role");
        string name = getJsonVal(body, "organizationName");
        string location = getJsonVal(body, "location");
        string specialties = getJsonVal(body, "specialties");
        string bedsText = getJsonVal(body, "availableBeds");
        string ambulanceType = getJsonVal(body, "ambulanceType");
        if (ambulanceType.empty()) ambulanceType = "BLS";
        int beds = 0;
        try { beds = bedsText.empty() ? 0 : stoi(bedsText); } catch (...) { beds = -1; }

        if ((role != "hospital" && role != "ambulance_provider") || name.empty() || location.empty() || beds < 0) {
            response = makeHttpResponse("400 Bad Request", "application/json",
                "{\"status\":\"error\",\"message\":\"Provide role, organizationName, location and valid availableBeds\"}");
        } else {
            lock_guard<mutex> lock(engine.dbMutex);
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "INSERT INTO organization_registrations(role, organization_name, location, specialties, available_beds, ambulance_type, status) VALUES(?,?,?,?,?,?, 'PENDING');";
            if (sqlite3_prepare_v2(engine.db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, role.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, location.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, specialties.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 5, beds);
                sqlite3_bind_text(stmt, 6, ambulanceType.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_DONE) {
                    int id = (int)sqlite3_last_insert_rowid(engine.db);
                    response = makeHttpResponse("201 Created", "application/json",
                        "{\"status\":\"pending\",\"registrationId\":" + to_string(id) + ",\"message\":\"Registration submitted for administrator verification\"}");
                } else {
                    response = makeHttpResponse("500 Internal Server Error", "application/json", "{\"status\":\"error\",\"message\":\"Could not save registration\"}");
                }
            } else {
                response = makeHttpResponse("500 Internal Server Error", "application/json", "{\"status\":\"error\",\"message\":\"Registration database error\"}");
            }
            if (stmt) sqlite3_finalize(stmt);
        }
    }
    // Administrator-only: list pending registrations.
    else if (request.find("GET /api/admin/registrations") != string::npos) {
        if (getHeaderVal(request, "X-Admin-Key") != adminSecret()) {
            response = makeHttpResponse("401 Unauthorized", "application/json", "{\"status\":\"error\",\"message\":\"Invalid admin key\"}");
        } else {
            lock_guard<mutex> lock(engine.dbMutex);
            sqlite3_stmt* stmt = nullptr;
            string json = "{\"registrations\":[";
            bool first = true;
            const char* sql = "SELECT id, role, organization_name, location, specialties, available_beds, ambulance_type, status, created_at FROM organization_registrations ORDER BY id DESC;";
            if (sqlite3_prepare_v2(engine.db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (!first) json += ",";
                    first = false;
                    auto col = [&](int i) { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i)); return string(v ? v : ""); };
                    json += "{\"id\":" + to_string(sqlite3_column_int(stmt, 0)) +
                        ",\"role\":" + escapeJson(col(1)) +
                        ",\"organizationName\":" + escapeJson(col(2)) +
                        ",\"location\":" + escapeJson(col(3)) +
                        ",\"specialties\":" + escapeJson(col(4)) +
                        ",\"availableBeds\":" + to_string(sqlite3_column_int(stmt, 5)) +
                        ",\"ambulanceType\":" + escapeJson(col(6)) +
                        ",\"status\":" + escapeJson(col(7)) +
                        ",\"createdAt\":" + escapeJson(col(8)) + "}";
                }
            }
            if (stmt) sqlite3_finalize(stmt);
            json += "]}";
            response = makeHttpResponse("200 OK", "application/json", json);
        }
    }
    // Administrator-only: approve a registration and activate its resource.
    else if (request.find("POST /api/admin/registrations/approve") != string::npos) {
        if (getHeaderVal(request, "X-Admin-Key") != adminSecret()) {
            response = makeHttpResponse("401 Unauthorized", "application/json", "{\"status\":\"error\",\"message\":\"Invalid admin key\"}");
        } else {
            size_t bodyPos = request.find("\r\n\r\n");
            string body = bodyPos == string::npos ? "" : request.substr(bodyPos + 4);
            int registrationId = 0;
            try { registrationId = stoi(getJsonVal(body, "registrationId")); } catch (...) { registrationId = 0; }
            string responseBody;
            string responseStatus = "400 Bad Request";
            string apiKey;
            {
                lock_guard<mutex> lock(engine.dbMutex);
                sqlite3_stmt* stmt = nullptr;
                const char* sql = "SELECT role, organization_name, location, specialties, available_beds, ambulance_type, status FROM organization_registrations WHERE id=?;";
                if (registrationId > 0 && sqlite3_prepare_v2(engine.db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(stmt, 1, registrationId);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        auto col = [&](int i) { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i)); return string(v ? v : ""); };
                        string role = col(0), name = col(1), location = col(2), specialties = col(3), status = col(6), type = col(5);
                        int beds = sqlite3_column_int(stmt, 4);
                        if (status == "PENDING") {
                            string resourceId = (role == "hospital" ? "Hosp_Reg_" : "Amb_Reg_") + to_string(registrationId);
                            apiKey = newApiKey();
                            char* err = nullptr;
                            if (role == "hospital") {
                                string insert = "INSERT INTO hospitals(id,name,location,specialties,available_beds) VALUES('" + sqlQuote(resourceId) + "','" + sqlQuote(name) + "','" + sqlQuote(location) + "','" + sqlQuote(specialties) + "'," + to_string(beds) + ");";
                                if (sqlite3_exec(engine.db, insert.c_str(), nullptr, nullptr, &err) == SQLITE_OK) {
                                    string upd = "UPDATE organization_registrations SET status='APPROVED', api_key='" + sqlQuote(apiKey) + "', resource_id='" + sqlQuote(resourceId) + "' WHERE id=" + to_string(registrationId) + ";";
                                    sqlite3_exec(engine.db, upd.c_str(), nullptr, nullptr, nullptr);
                                    responseStatus = "200 OK";
                                    responseBody = "{\"status\":\"approved\",\"resourceId\":" + escapeJson(resourceId) + ",\"apiKey\":" + escapeJson(apiKey) + ",\"message\":\"Store this API key securely; it is shown only on approval\"}";
                                } else responseBody = "{\"status\":\"error\",\"message\":\"Could not create hospital resource (ID may already exist)\"}";
                            } else {
                                string insert = "INSERT INTO ambulances(id,location,is_available,type) VALUES('" + sqlQuote(resourceId) + "','" + sqlQuote(location) + "',1,'" + sqlQuote(type) + "');";
                                if (sqlite3_exec(engine.db, insert.c_str(), nullptr, nullptr, &err) == SQLITE_OK) {
                                    string upd = "UPDATE organization_registrations SET status='APPROVED', api_key='" + sqlQuote(apiKey) + "', resource_id='" + sqlQuote(resourceId) + "' WHERE id=" + to_string(registrationId) + ";";
                                    sqlite3_exec(engine.db, upd.c_str(), nullptr, nullptr, nullptr);
                                    responseStatus = "200 OK";
                                    responseBody = "{\"status\":\"approved\",\"resourceId\":" + escapeJson(resourceId) + ",\"apiKey\":" + escapeJson(apiKey) + ",\"message\":\"Store this API key securely; it is shown only on approval\"}";
                                } else responseBody = "{\"status\":\"error\",\"message\":\"Could not create ambulance resource (ID may already exist)\"}";
                            }
                            if (err) sqlite3_free(err);
                        } else {
                            responseStatus = "409 Conflict";
                            responseBody = "{\"status\":\"error\",\"message\":\"Registration is not pending\"}";
                        }
                    } else {
                        responseStatus = "404 Not Found";
                        responseBody = "{\"status\":\"error\",\"message\":\"Registration not found\"}";
                    }
                } else responseBody = "{\"status\":\"error\",\"message\":\"Invalid registrationId\"}";
                if (stmt) sqlite3_finalize(stmt);
            }
            if (responseBody.empty()) responseBody = "{\"status\":\"error\",\"message\":\"Approval failed\"}";
            if (responseStatus == "200 OK") engine.loadDataFromDatabase();
            response = makeHttpResponse(responseStatus, "application/json", responseBody);
        }
    }
    // Hospital/provider resource updates: API key must belong to an approved registration.
    else if (request.find("POST /api/provider/resource") != string::npos) {
        string auth = getHeaderVal(request, "Authorization");
        const string prefix = "Bearer ";
        string key = auth.rfind(prefix, 0) == 0 ? auth.substr(prefix.size()) : "";
        size_t bodyPos = request.find("\r\n\r\n");
        string body = bodyPos == string::npos ? "" : request.substr(bodyPos + 4);
        string location = getJsonVal(body, "location");
        string availText = getJsonVal(body, "isAvailable");
        string bedsText = getJsonVal(body, "availableBeds");
        string responseBody = "{\"status\":\"error\",\"message\":\"Invalid or inactive API key\"}";
        string responseStatus = "401 Unauthorized";
        bool reload = false;
        {
            lock_guard<mutex> lock(engine.dbMutex);
            sqlite3_stmt* stmt = nullptr;
            if (!key.empty() && sqlite3_prepare_v2(engine.db, "SELECT role, resource_id FROM organization_registrations WHERE api_key=? AND status='APPROVED';", -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* roleText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    const char* idText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    string role = roleText ? roleText : "", id = idText ? idText : "";
                    if (role == "hospital") {
                        string fields;
                        if (!location.empty()) fields += "location='" + sqlQuote(location) + "',";
                        if (!bedsText.empty()) {
                            try { int beds = stoi(bedsText); if (beds >= 0) fields += "available_beds=" + to_string(beds) + ","; } catch (...) {}
                        }
                        if (!fields.empty()) {
                            fields.pop_back();
                            string upd = "UPDATE hospitals SET " + fields + " WHERE id='" + sqlQuote(id) + "';";
                            if (sqlite3_exec(engine.db, upd.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK) {
                                responseStatus = "200 OK"; responseBody = "{\"status\":\"success\",\"message\":\"Hospital resource updated\"}"; reload = true;
                            } else { responseStatus = "500 Internal Server Error"; responseBody = "{\"status\":\"error\",\"message\":\"Update failed\"}"; }
                        } else { responseStatus = "400 Bad Request"; responseBody = "{\"status\":\"error\",\"message\":\"No valid fields supplied\"}"; }
                    } else if (role == "ambulance_provider") {
                        string fields;
                        if (!location.empty()) fields += "location='" + sqlQuote(location) + "',";
                        if (availText == "true" || availText == "1") fields += "is_available=1,";
                        else if (availText == "false" || availText == "0") fields += "is_available=0,";
                        if (!fields.empty()) {
                            fields.pop_back();
                            string upd = "UPDATE ambulances SET " + fields + " WHERE id='" + sqlQuote(id) + "';";
                            if (sqlite3_exec(engine.db, upd.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK) {
                                responseStatus = "200 OK"; responseBody = "{\"status\":\"success\",\"message\":\"Ambulance resource updated\"}"; reload = true;
                            } else { responseStatus = "500 Internal Server Error"; responseBody = "{\"status\":\"error\",\"message\":\"Update failed\"}"; }
                        } else { responseStatus = "400 Bad Request"; responseBody = "{\"status\":\"error\",\"message\":\"No valid fields supplied\"}"; }
                    }
                }
            }
            if (stmt) sqlite3_finalize(stmt);
        }
        if (reload) engine.loadDataFromDatabase();
        response = makeHttpResponse(responseStatus, "application/json", responseBody);
    }
    // Administrator dashboard summary.
    else if (request.find("GET /api/admin/summary") != string::npos) {
        if (getHeaderVal(request, "X-Admin-Key") != adminSecret()) {
            response = makeHttpResponse("401 Unauthorized", "application/json", "{\"status\":\"error\",\"message\":\"Invalid admin key\"}");
        } else {
            lock_guard<mutex> lock(engine.dbMutex);
            auto count = [&](const char* sql) { sqlite3_stmt* st=nullptr; int n=0; if (sqlite3_prepare_v2(engine.db, sql, -1, &st, nullptr)==SQLITE_OK && sqlite3_step(st)==SQLITE_ROW) n=sqlite3_column_int(st,0); if(st) sqlite3_finalize(st); return n; };
            string json = "{\"pendingRegistrations\":" + to_string(count("SELECT COUNT(*) FROM organization_registrations WHERE status='PENDING';")) +
                ",\"hospitals\":" + to_string(count("SELECT COUNT(*) FROM hospitals;")) +
                ",\"ambulances\":" + to_string(count("SELECT COUNT(*) FROM ambulances;")) +
                ",\"availableAmbulances\":" + to_string(count("SELECT COUNT(*) FROM ambulances WHERE is_available=1;")) +
                ",\"activeEmergencyLogs\":" + to_string(count("SELECT COUNT(*) FROM emergency_logs WHERE assigned_ambulance NOT LIKE 'RESOLVED (%';")) + "}";
            response = makeHttpResponse("200 OK", "application/json", json);
        }
    }

    if (response.empty()) {
    // Existing emergency/dispatch API routes
    if (request.find("GET / ") != string::npos || request.find("GET /index.html") != string::npos ||
        request.find("GET /admin.html") != string::npos || request.find("GET /register.html") != string::npos) {
        string filePath = "static/index.html";
        if (request.find("GET /admin.html") != string::npos) filePath = "static/admin.html";
        else if (request.find("GET /register.html") != string::npos) filePath = "static/register.html";
        ifstream file(filePath);
        if (file) {
            stringstream ss;
            ss << file.rdbuf();
            response = makeHttpResponse("200 OK", "text/html; charset=UTF-8", ss.str());
        } else {
            response = makeHttpResponse("404 Not Found", "text/plain", "File Not Found");
        }
    }
    // POST /api/emergency
    else if (request.find("POST /api/emergency") != string::npos) {
        size_t bodyPos = request.find("\r\n\r\n");
        string reqBody = (bodyPos != string::npos) ? request.substr(bodyPos + 4) : "";

        Emergency e;
        {
            lock_guard<mutex> lock(engine.queueMutex);
            e.id = engine.emergencyCounter++;
            e.patientName = getJsonVal(reqBody, "patientName");

            string sevStr = getJsonVal(reqBody, "severity");
            try {
                e.severity = sevStr.empty() ? 1 : stoi(sevStr);
            } catch (...) {
                e.severity = 1;
            }

            e.requiredSpecialty = getJsonVal(reqBody, "requiredSpecialty");
            e.location = getJsonVal(reqBody, "location");
            e.timestamp = chrono::duration_cast<chrono::milliseconds>(
                chrono::system_clock::now().time_since_epoch()
            ).count();

            engine.emergencyQueue.push(e);
        }

        string resJson = "{\"status\":\"success\",\"message\":\"Emergency Registered\",\"id\":" + to_string(e.id) + "}";
        response = makeHttpResponse("200 OK", "application/json", resJson);
    }
    // POST /api/dispatch
    else if (request.find("POST /api/dispatch") != string::npos) {
        Emergency e;
        bool hasQueue = true;

        {
            lock_guard<mutex> lock(engine.queueMutex);
            if (engine.emergencyQueue.empty()) {
                hasQueue = false;
            } else {
                e = engine.emergencyQueue.top();
                engine.emergencyQueue.pop();
            }
        }

        string resJson;
        if (!hasQueue) {
            resJson = "{\"status\":\"empty\",\"message\":\"No pending emergencies\"}";
        } else {
            string bestAmb;
            int minAmbTime = INT_MAX;
            vector<string> ambRoute;

            {
                lock_guard<mutex> lock(engine.ambulanceMutex);
                for (auto& [id, amb] : engine.ambulances) {
                    if (!amb.isAvailable) continue;

                    auto routeRes = engine.dijkstra(amb.location, e.location);
                    if (routeRes.first < minAmbTime) {
                        minAmbTime = routeRes.first;
                        bestAmb = id;
                        ambRoute = std::move(routeRes.second);
                    }
                }

                if (!bestAmb.empty()) {
                    engine.ambulances[bestAmb].isAvailable = false;
                }
            }

            string bestHosp;
            int minHospTime = INT_MAX;
            vector<string> hospRoute;

            if (!bestAmb.empty()) {
                lock_guard<mutex> lock(engine.hospitalMutex);
                for (auto& [id, hosp] : engine.hospitals) {
                    if (hosp.availableBeds <= 0 || !hosp.specialties.count(e.requiredSpecialty))
                        continue;

                    auto routeRes = engine.dijkstra(e.location, hosp.location);
                    if (routeRes.first < minHospTime) {
                        minHospTime = routeRes.first;
                        bestHosp = id;
                        hospRoute = std::move(routeRes.second);
                    }
                }

                if (!bestHosp.empty()) {
                    engine.hospitals[bestHosp].availableBeds--;
                }
            }

            if (bestAmb.empty() || bestHosp.empty()) {
                if (!bestAmb.empty()) {
                    lock_guard<mutex> lock(engine.ambulanceMutex);
                    engine.ambulances[bestAmb].isAvailable = true;
                }
                resJson = "{\"status\":\"FAILED\",\"message\":\"Resource unavailable\",\"patientName\":" + escapeJson(e.patientName) + "}";
            } else {
                engine.logDispatchToDatabase(e, bestAmb, bestHosp);
                string hospitalName = engine.hospitals[bestHosp].name;

                resJson = "{"
                    "\"emergencyId\":" + to_string(e.id) + ","
                    "\"patientName\":" + escapeJson(e.patientName) + ","
                    "\"severity\":" + to_string(e.severity) + ","
                    "\"location\":" + escapeJson(e.location) + ","
                    "\"ambulanceId\":" + escapeJson(bestAmb) + ","
                    "\"ambulanceETA\":" + to_string(minAmbTime == INT_MAX ? 0 : minAmbTime) + ","
                    "\"ambulanceRoute\":" + vectorToJson(ambRoute) + ","
                    "\"hospitalId\":" + escapeJson(bestHosp) + ","
                    "\"hospitalName\":" + escapeJson(hospitalName) + ","
                    "\"hospitalETA\":" + to_string(minHospTime == INT_MAX ? 0 : minHospTime) + ","
                    "\"hospitalRoute\":" + vectorToJson(hospRoute) + ","
                    "\"status\":\"SUCCESS\""
                "}";
            }
        }

        response = makeHttpResponse("200 OK", "application/json", resJson);
    }
    // POST /api/resolve
    else if (request.find("POST /api/resolve") != string::npos) {
        size_t bodyPos = request.find("\r\n\r\n");
        string reqBody = (bodyPos != string::npos) ? request.substr(bodyPos + 4) : "";
        string idStr = getJsonVal(reqBody, "emergencyId");

        int emergencyId = 0;
        try {
            emergencyId = stoi(idStr);
        } catch (...) {
            emergencyId = 0;
        }

        bool resolved = (emergencyId > 0) && engine.resolveEmergency(emergencyId);
        string resJson = resolved
            ? "{\"status\":\"success\",\"message\":\"Emergency resolved and resources released\"}"
            : "{\"status\":\"error\",\"message\":\"Emergency not found or already resolved\"}";

        response = makeHttpResponse(resolved ? "200 OK" : "404 Not Found", "application/json", resJson);
    }
    // POST /api/reset
    else if (request.find("POST /api/reset") != string::npos) {
        if (getHeaderVal(request, "X-Admin-Key") != adminSecret()) {
            response = makeHttpResponse("401 Unauthorized", "application/json", "{\"status\":\"error\",\"message\":\"Invalid admin key\"}");
        } else {
            engine.autoResetOnStartup();
            engine.loadDataFromDatabase();
            string resJson = "{\"status\":\"success\",\"message\":\"System reset. Registration records were retained.\"}";
            response = makeHttpResponse("200 OK", "application/json", resJson);
        }
    }
    else {
        response = makeHttpResponse("404 Not Found", "text/plain", "Route Not Found");
    }

    }
    if (response.empty()) response = makeHttpResponse("404 Not Found", "text/plain", "Route Not Found");
    send(clientSocket, response.c_str(), response.size(), 0);
    close(clientSocket);
}

// --- MAIN SERVER ---
int main() {
    ios_base::sync_with_stdio(false);
    cin.tie(NULL);

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        cerr << "Failed to create socket.\n";
        return 1;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    const char* portValue = std::getenv("PORT");
    int port = 18080;
    if (portValue && *portValue) {
        try { port = std::stoi(portValue); } catch (...) {
            cerr << "Invalid PORT value.\n";
            close(serverFd);
            return 1;
        }
    }
    address.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(serverFd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        cerr << "Bind failed.\n";
        close(serverFd);
        return 1;
    }

    if (listen(serverFd, 10) < 0) {
        cerr << "Listen failed.\n";
        close(serverFd);
        return 1;
    }

    cout << "MedEvac server running at http://localhost:18080\n";
    if (adminSecret() == "dev-only-change-me")
        cerr << "WARNING: using development admin key. Set MEDEVAC_ADMIN_KEY before any shared/public demo.\n";

    while (true) {
        int clientSocket = accept(serverFd, nullptr, nullptr);
        if (clientSocket >= 0) {
            thread(handleClient, clientSocket).detach();
        }
    }

    close(serverFd);
    return 0;
}