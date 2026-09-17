#include <crow.h>
#include <sqlite3.h>
#include <sodium.h>

#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

sqlite3* db = nullptr;

// ---------- small helpers ----------

std::string read_file(const std::string& path)
{
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string replace_all(std::string text,
                        const std::string& key,
                        const std::string& value)
{
    size_t pos = text.find(key);

    while (pos != std::string::npos)
    {
        text.replace(pos, key.size(), value);
        pos = text.find(key, pos + value.size());
    }

    return text;
}

// Stops a student from breaking the page with characters like < or >.
std::string escape_html(const std::string& text)
{
    std::string out;

    for (char c : text)
    {
        if (c == '&')      out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else               out += c;
    }

    return out;
}

// Turns a web address inside a post into a link you can click. It runs after
// escape_html, and only http and https are accepted, so no other kind of
// address can be smuggled in.
std::string linkify(const std::string& text)
{
    std::string out;
    size_t i = 0;

    while (i < text.size())
    {
        bool is_link = text.compare(i, 7, "http://") == 0
                    || text.compare(i, 8, "https://") == 0;

        if (!is_link)
        {
            out += text[i];
            i++;
            continue;
        }

        size_t end = i;

        while (end < text.size()
               && text[end] != ' '
               && text[end] != '\n'
               && text[end] != '<'
               && text[end] != '"')
        {
            end++;
        }

        std::string url = text.substr(i, end - i);

        out += "<a href=\"" + url + "\" target=\"_blank\" "
               "rel=\"noopener noreferrer\">" + url + "</a>";

        i = end;
    }

    return out;
}

crow::response html_page(const std::string& html)
{
    crow::response res(200, html);
    res.set_header("Content-Type", "text/html; charset=utf-8");
    return res;
}

crow::response redirect_to(const std::string& path)
{
    crow::response res(302);
    res.set_header("Location", path);
    return res;
}

std::string get_cookie(const crow::request& req, const std::string& name)
{
    std::string cookies = req.get_header_value("Cookie");
    std::string key = name + "=";

    size_t start = cookies.find(key);

    if (start == std::string::npos)
    {
        return "";
    }

    start += key.size();
    size_t end = cookies.find(';', start);

    return cookies.substr(start, end - start);
}

// Turns a password into an Argon2 hash. The real password is never stored.
std::string hash_password(const std::string& password)
{
    char hash[crypto_pwhash_STRBYTES];

    if (crypto_pwhash_str(hash,
                          password.c_str(),
                          password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
    {
        return "";
    }

    return std::string(hash);
}

bool password_matches(const std::string& stored, const std::string& password)
{
    return crypto_pwhash_str_verify(stored.c_str(),
                                    password.c_str(),
                                    password.size()) == 0;
}

// A long random string that nobody can guess.
std::string make_session_token()
{
    unsigned char bytes[32];
    char text[65];

    randombytes_buf(bytes, sizeof bytes);
    sodium_bin2hex(text, sizeof text, bytes, sizeof bytes);

    return std::string(text);
}

// Only letters, digits and underscore, because the username is also
// used in the file names of the uploads.
bool is_valid_username(const std::string& username)
{
    if (username.empty() || username.size() > 20)
    {
        return false;
    }

    for (char c : username)
    {
        bool ok = (c >= 'a' && c <= 'z')
               || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9')
               || c == '_';

        if (!ok)
        {
            return false;
        }
    }

    return true;
}

std::string lowercase(std::string text)
{
    for (char& c : text)
    {
        c = std::tolower(c);
    }

    return text;
}

// Reads the original file name the browser sent with an uploaded file.
std::string uploaded_file_name(const crow::multipart::part& file)
{
    auto params = file.get_header_object("Content-Disposition").params;
    auto it = params.find("filename");

    return (it != params.end()) ? it->second : "";
}

std::string picture_extension(const std::string& file_name)
{
    size_t dot = file_name.rfind('.');

    if (dot == std::string::npos)
    {
        return "";
    }

    std::string extension = lowercase(file_name.substr(dot));

    if (extension == ".jpg" || extension == ".jpeg" || extension == ".png")
    {
        return extension;
    }

    return "";
}

bool is_pdf(const std::string& file_name)
{
    size_t dot = file_name.rfind('.');

    return dot != std::string::npos && lowercase(file_name.substr(dot)) == ".pdf";
}

// The time is part of the name so that when you change your picture the
// browser loads the new one instead of the old one it has saved.
std::string photo_file_name(const std::string& username,
                            const std::string& extension)
{
    return username + "_" + std::to_string(std::time(nullptr)) + extension;
}

// Uploads bigger than this are refused, so one visitor cannot fill the disk.
const size_t MAX_UPLOAD_BYTES = 5 * 1024 * 1024;

bool upload_too_big(const crow::multipart::part& file)
{
    return file.body.size() > MAX_UPLOAD_BYTES;
}

// Saves an uploaded file inside static/ so the browser can open it,
// and gives back the address to store in the database.
std::string save_upload(const crow::multipart::part& file,
                        const std::string& saved_name)
{
    std::ofstream out("static/uploads/" + saved_name, std::ios::binary);
    out << file.body;
    out.close();

    return "/static/uploads/" + saved_name;
}

// ---------- database ----------

struct Student
{
    int id = 0;
    std::string username;
    std::string bio;
    std::string photo;
    std::string year;
    std::string program;
    std::string marks;
    int is_private = 0;
    int dm_open = 1;
};

std::string column_text(sqlite3_stmt* stmt, int index)
{
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : "";
}

// ---------- sessions ----------

void create_session(const std::string& token, int student_id)
{
    const char* sql = "INSERT INTO sessions(token, student_id) VALUES(?, ?);";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, student_id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void delete_session(const std::string& token)
{
    const char* sql = "DELETE FROM sessions WHERE token = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// Looks the cookie up in the sessions table. An invented cookie finds no row,
// so it gives 0 and the visitor stays logged out.
int logged_in_id(const crow::request& req)
{
    std::string token = get_cookie(req, "session");

    if (token.empty())
    {
        return 0;
    }

    const char* sql = "SELECT student_id FROM sessions WHERE token = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);

    int id = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        id = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return id;
}

bool username_taken(const std::string& username)
{
    const char* sql = "SELECT id FROM students WHERE username = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);

    bool found = sqlite3_step(stmt) == SQLITE_ROW;

    sqlite3_finalize(stmt);
    return found;
}

bool add_student(const Student& student, const std::string& password)
{
    const char* sql =
        "INSERT INTO students(username, password, bio, photo, year, program, marks) "
        "VALUES(?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    sqlite3_bind_text(stmt, 1, student.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, student.bio.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, student.photo.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, student.year.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, student.program.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, student.marks.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;

    sqlite3_finalize(stmt);
    return ok;
}

// Saves the edited profile. The username is not changed, because it is the
// name other students know you by and it is used in the upload file names.
void update_student(const Student& student)
{
    const char* sql =
        "UPDATE students SET bio = ?, year = ?, program = ?, marks = ?, photo = ?, "
        "is_private = ?, dm_open = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    sqlite3_bind_text(stmt, 1, student.bio.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, student.year.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, student.program.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, student.marks.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, student.photo.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, student.is_private);
    sqlite3_bind_int(stmt, 7, student.dm_open);
    sqlite3_bind_int(stmt, 8, student.id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool get_student(int id, Student& student)
{
    const char* sql =
        "SELECT id, username, bio, photo, year, program, marks, is_private, "
        "dm_open FROM students WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);

    bool found = false;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        student.id       = sqlite3_column_int(stmt, 0);
        student.username = column_text(stmt, 1);
        student.bio      = column_text(stmt, 2);
        student.photo    = column_text(stmt, 3);
        student.year     = column_text(stmt, 4);
        student.program  = column_text(stmt, 5);
        student.marks      = column_text(stmt, 6);
        student.is_private = sqlite3_column_int(stmt, 7);
        student.dm_open    = sqlite3_column_int(stmt, 8);
        found = true;
    }

    sqlite3_finalize(stmt);
    return found;
}

// Checks the login details and gives back the student id, or 0.
void set_password(int student_id, const std::string& hash)
{
    const char* sql = "UPDATE students SET password = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, student_id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int check_login(const std::string& username, const std::string& password)
{
    const char* sql = "SELECT id, password FROM students WHERE username = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);

    int id = 0;
    std::string stored;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        id = sqlite3_column_int(stmt, 0);
        stored = column_text(stmt, 1);
    }

    sqlite3_finalize(stmt);

    if (id == 0)
    {
        return 0;
    }

    // Accounts made before passwords were hashed still hold plain text.
    // They are checked once and then written back as a hash.
    if (stored.rfind("$argon2", 0) != 0)
    {
        if (stored != password)
        {
            return 0;
        }

        set_password(id, hash_password(password));
        return id;
    }

    return password_matches(stored, password) ? id : 0;
}

void add_document(int student_id,
                     const std::string& title,
                     const std::string& file_path)
{
    const char* sql =
        "INSERT INTO documents(student_id, title, file_path) VALUES(?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    sqlite3_bind_int(stmt, 1, student_id);
    sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, file_path.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// Saves the PDF and the database row together. Used by signup and by the
// "add document" form on your own profile.
// A document can be a PDF or a picture, because a marksheet is often just
// a photo of a piece of paper.
std::string document_extension(const std::string& file_name)
{
    if (is_pdf(file_name))
    {
        return ".pdf";
    }

    return picture_extension(file_name);
}

bool save_document(int student_id,
                   const std::string& username,
                   const std::string& title,
                   const crow::multipart::part& file)
{
    std::string extension = document_extension(uploaded_file_name(file));

    if (title.empty() || file.body.empty() || extension.empty()
        || upload_too_big(file))
    {
        return false;
    }

    // The time is part of the name so a new document never overwrites
    // one that is already saved.
    std::string saved_name =
        username + "_doc" + std::to_string(std::time(nullptr)) + extension;

    add_document(student_id, title, save_upload(file, saved_name));
    return true;
}

// Finds a document, but only if it belongs to this student. This is what
// stops someone deleting another student's document.
bool find_own_document(int document_id,
                          int student_id,
                          std::string& file_path)
{
    const char* sql =
        "SELECT file_path FROM documents WHERE id = ? AND student_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, document_id);
    sqlite3_bind_int(stmt, 2, student_id);

    bool found = false;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        file_path = column_text(stmt, 0);
        found = true;
    }

    sqlite3_finalize(stmt);
    return found;
}

void delete_document(int document_id, int student_id)
{
    const char* sql = "DELETE FROM documents WHERE id = ? AND student_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, document_id);
    sqlite3_bind_int(stmt, 2, student_id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ---------- posts ----------

// A post is a short update. The file is optional.
bool add_post(int student_id,
              const std::string& username,
              const std::string& body,
              const crow::multipart::part& file)
{
    if (body.empty() || body.size() > 2000)
    {
        return false;
    }

    std::string web_path;

    if (!file.body.empty())
    {
        std::string extension = document_extension(uploaded_file_name(file));

        if (extension.empty() || upload_too_big(file))
        {
            return false;
        }

        web_path = save_upload(file,
            username + "_post" + std::to_string(std::time(nullptr)) + extension);
    }

    const char* sql =
        "INSERT INTO posts(student_id, body, file_path) VALUES(?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, student_id);
    sqlite3_bind_text(stmt, 2, body.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, web_path.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return true;
}

bool find_own_post(int post_id, int student_id, std::string& file_path)
{
    const char* sql =
        "SELECT file_path FROM posts WHERE id = ? AND student_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, student_id);

    bool found = false;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        file_path = column_text(stmt, 0);
        found = true;
    }

    sqlite3_finalize(stmt);
    return found;
}

void delete_post(int post_id, int student_id)
{
    const char* sql = "DELETE FROM posts WHERE id = ? AND student_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, student_id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ---------- following ----------

// Returns "accepted", "pending", or "" when there is no follow at all.
std::string follow_status(int follower_id, int following_id)
{
    const char* sql =
        "SELECT status FROM follows WHERE follower_id = ? AND following_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, follower_id);
    sqlite3_bind_int(stmt, 2, following_id);

    std::string status;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        status = column_text(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return status;
}

// A public profile is followed straight away. A private one has to be
// accepted by its owner first, so the follow starts as "pending".
void start_follow(int follower_id, int following_id, bool target_is_private)
{
    const char* sql =
        "INSERT OR IGNORE INTO follows(follower_id, following_id, status) "
        "VALUES(?, ?, ?);";

    std::string status = target_is_private ? "pending" : "accepted";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, follower_id);
    sqlite3_bind_int(stmt, 2, following_id);
    sqlite3_bind_text(stmt, 3, status.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// Used for unfollowing, for cancelling your own request, and for rejecting
// somebody else's request.
void remove_follow(int follower_id, int following_id)
{
    const char* sql =
        "DELETE FROM follows WHERE follower_id = ? AND following_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, follower_id);
    sqlite3_bind_int(stmt, 2, following_id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void accept_follow(int follower_id, int following_id)
{
    const char* sql =
        "UPDATE follows SET status = 'accepted' "
        "WHERE follower_id = ? AND following_id = ? AND status = 'pending';";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, follower_id);
    sqlite3_bind_int(stmt, 2, following_id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int count_follows(int student_id, const char* sql)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, student_id);

    int total = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        total = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return total;
}

int count_followers(int student_id)
{
    return count_follows(student_id,
        "SELECT COUNT(*) FROM follows "
        "WHERE following_id = ? AND status = 'accepted';");
}

int count_following(int student_id)
{
    return count_follows(student_id,
        "SELECT COUNT(*) FROM follows "
        "WHERE follower_id = ? AND status = 'accepted';");
}

// This is the rule that decides what a visitor is allowed to see.
bool can_see_details(const Student& student, int viewer_id)
{
    if (!student.is_private)
    {
        return true;
    }

    if (viewer_id == student.id)
    {
        return true;
    }

    return follow_status(viewer_id, student.id) == "accepted";
}

// ---------- direct messages ----------

// The rule for who is allowed to message whom. A student either has their
// inbox open to everybody, or only to the people who follow them.
bool can_message(int sender_id, const Student& receiver)
{
    if (sender_id == 0 || sender_id == receiver.id)
    {
        return false;
    }

    if (receiver.dm_open)
    {
        return true;
    }

    return follow_status(sender_id, receiver.id) == "accepted";
}

void add_message(int sender_id, int receiver_id, const std::string& body)
{
    const char* sql =
        "INSERT INTO messages(sender_id, receiver_id, body) VALUES(?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, sender_id);
    sqlite3_bind_int(stmt, 2, receiver_id);
    sqlite3_bind_text(stmt, 3, body.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// Everything the two of you have said to each other, oldest first.
std::string conversation_html(int me, int them)
{
    const char* sql =
        "SELECT sender_id, body, created_at FROM messages "
        "WHERE (sender_id = ? AND receiver_id = ?) "
        "   OR (sender_id = ? AND receiver_id = ?) "
        "ORDER BY id;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, me);
    sqlite3_bind_int(stmt, 2, them);
    sqlite3_bind_int(stmt, 3, them);
    sqlite3_bind_int(stmt, 4, me);

    std::string list;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        bool mine = sqlite3_column_int(stmt, 0) == me;
        std::string body = escape_html(column_text(stmt, 1));
        std::string when = escape_html(column_text(stmt, 2));

        list += "<div class=\"bubble-row ";
        list += mine ? "mine" : "theirs";
        list += "\">";
        list += "<div class=\"bubble\">";
        list += "<p>" + body + "</p>";
        list += "<span class=\"small\">" + when + "</span>";
        list += "</div>";
        list += "</div>";
    }

    sqlite3_finalize(stmt);

    if (list.empty())
    {
        return "<p class=\"empty\">No messages yet. Say hello.</p>";
    }

    return list;
}

// The inbox: one line per person you have messaged or who has messaged you.
std::string inbox_html(int me)
{
    const char* sql =
        "SELECT s.id, s.username, s.photo, "
        "       (SELECT body FROM messages m2 "
        "         WHERE (m2.sender_id = s.id AND m2.receiver_id = ?) "
        "            OR (m2.sender_id = ? AND m2.receiver_id = s.id) "
        "         ORDER BY m2.id DESC LIMIT 1) AS last_body "
        "FROM students s "
        "WHERE s.id IN ("
        "    SELECT receiver_id FROM messages WHERE sender_id = ? "
        "    UNION "
        "    SELECT sender_id FROM messages WHERE receiver_id = ?"
        ") ORDER BY s.username;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, me);
    sqlite3_bind_int(stmt, 2, me);
    sqlite3_bind_int(stmt, 3, me);
    sqlite3_bind_int(stmt, 4, me);

    std::string list;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        std::string id       = std::to_string(sqlite3_column_int(stmt, 0));
        std::string username = escape_html(column_text(stmt, 1));
        std::string photo    = escape_html(column_text(stmt, 2));
        std::string last     = escape_html(column_text(stmt, 3));

        list += "<a class=\"request\" href=\"/messages/" + id + "\">";
        list += "<span class=\"request-who\">";
        list += "<img class=\"thumb\" src=\"" + photo + "\" alt=\"\">";
        list += "<span><b>" + username + "</b><br>";
        list += "<span class=\"small\">" + last + "</span></span>";
        list += "</span>";
        list += "</a>";
    }

    sqlite3_finalize(stmt);

    if (list.empty())
    {
        return "<p class=\"empty\">No conversations yet. Open somebody's "
               "profile and press Message.</p>";
    }

    return list;
}

// ---------- building pieces of HTML ----------

std::string nav_html(int user_id)
{
    std::string links;

    links += "<a href=\"/\">Home</a>";
    links += "<a href=\"/search\">Search</a>";

    if (user_id == 0)
    {
        links += "<a href=\"/login\">Login</a>";
        links += "<a class=\"nav-button\" href=\"/signup\">Sign Up</a>";
    }
    else
    {
        links += "<a href=\"/messages\">Messages</a>";
        links += "<a href=\"/profile\">My Profile</a>";
        links += "<a class=\"nav-button\" href=\"/logout\">Logout</a>";
    }

    return links;
}

// Turns the rows of a query into a grid of student cards.
// The query must select: id, username, photo, year, program, is_private.
// A private student only shows a picture and a name, the same as their profile.
std::string render_cards(sqlite3_stmt* stmt, int viewer_id)
{
    std::string cards;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int student_id       = sqlite3_column_int(stmt, 0);
        std::string id       = std::to_string(student_id);
        std::string username = escape_html(column_text(stmt, 1));
        std::string photo    = escape_html(column_text(stmt, 2));
        std::string year     = escape_html(column_text(stmt, 3));
        std::string program  = escape_html(column_text(stmt, 4));
        int is_private       = sqlite3_column_int(stmt, 5);

        bool show_details = !is_private
                         || viewer_id == student_id
                         || follow_status(viewer_id, student_id) == "accepted";

        cards += "<a class=\"card\" href=\"/student/" + id + "\">";
        cards += "<img class=\"card-photo\" src=\"" + photo + "\" alt=\"\">";
        cards += "<h3>" + username + "</h3>";

        if (show_details)
        {
            cards += "<p class=\"muted\">" + program + "</p>";

            if (!year.empty())
            {
                cards += "<span class=\"chip\">" + year + "</span>";
            }
        }
        else
        {
            cards += "<p class=\"muted\">Private profile</p>";
        }

        cards += "</a>";
    }

    return cards;
}

std::string newest_students_html(int viewer_id)
{
    const char* sql =
        "SELECT id, username, photo, year, program, is_private FROM students "
        "ORDER BY id DESC LIMIT 8;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    std::string cards = render_cards(stmt, viewer_id);

    sqlite3_finalize(stmt);

    if (cards.empty())
    {
        return "<p class=\"empty\">No students have joined yet.</p>";
    }

    return "<div class=\"grid\">" + cards + "</div>";
}

// Recommends students doing the same program as you.
std::string recommended_html(int user_id)
{
    Student me;

    if (user_id == 0 || !get_student(user_id, me) || me.program.empty())
    {
        return "";
    }

    const char* sql =
        "SELECT id, username, photo, year, program, is_private FROM students "
        "WHERE program = ? AND id != ? AND is_private = 0 LIMIT 8;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, me.program.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, user_id);

    std::string cards = render_cards(stmt, user_id);

    sqlite3_finalize(stmt);

    if (cards.empty())
    {
        return "";
    }

    std::string section;
    section += "<h2>Also studying " + escape_html(me.program) + "</h2>";
    section += "<p class=\"section-note\">Students on the same program as you.</p>";
    section += "<div class=\"grid\">" + cards + "</div>";

    return section;
}

// Builds the little rounded labels, skipping any field the student left blank.
// The Follow / Requested / Unfollow button shown on other people's profiles.
std::string follow_button_html(const Student& student, int viewer_id)
{
    if (viewer_id == student.id)
    {
        return "";
    }

    if (viewer_id == 0)
    {
        return "<p class=\"muted\"><a href=\"/login\">Log in</a> to follow "
             + escape_html(student.username) + ".</p>";
    }

    std::string status = follow_status(viewer_id, student.id);
    std::string id = std::to_string(student.id);
    std::string form;

    if (status.empty())
    {
        form += "<form action=\"/follow\" method=\"post\">";
        form += "<input type=\"hidden\" name=\"id\" value=\"" + id + "\">";
        form += "<button type=\"submit\">Follow</button>";
        form += "</form>";
    }
    else if (status == "pending")
    {
        form += "<form action=\"/unfollow\" method=\"post\">";
        form += "<input type=\"hidden\" name=\"id\" value=\"" + id + "\">";
        form += "<button class=\"quiet-button\" type=\"submit\">"
                "Requested - cancel</button>";
        form += "</form>";
    }
    else
    {
        form += "<form action=\"/unfollow\" method=\"post\">";
        form += "<input type=\"hidden\" name=\"id\" value=\"" + id + "\">";
        form += "<button class=\"quiet-button\" type=\"submit\">Unfollow</button>";
        form += "</form>";
    }

    return form;
}

// The list of people waiting for you to accept them, shown on your own profile.
std::string follow_requests_html(int student_id)
{
    const char* sql =
        "SELECT s.id, s.username, s.photo FROM follows f "
        "JOIN students s ON s.id = f.follower_id "
        "WHERE f.following_id = ? AND f.status = 'pending' ORDER BY f.id;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, student_id);

    std::string list;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        std::string id       = std::to_string(sqlite3_column_int(stmt, 0));
        std::string username = escape_html(column_text(stmt, 1));
        std::string photo    = escape_html(column_text(stmt, 2));

        list += "<div class=\"request\">";
        list += "<a class=\"request-who\" href=\"/student/" + id + "\">";
        list += "<img class=\"thumb\" src=\"" + photo + "\" alt=\"\">";
        list += "<span>" + username + "</span>";
        list += "</a>";
        list += "<div class=\"request-buttons\">";
        list += "<form action=\"/follow/accept\" method=\"post\">";
        list += "<input type=\"hidden\" name=\"id\" value=\"" + id + "\">";
        list += "<button type=\"submit\">Accept</button>";
        list += "</form>";
        list += "<form action=\"/follow/reject\" method=\"post\">";
        list += "<input type=\"hidden\" name=\"id\" value=\"" + id + "\">";
        list += "<button class=\"delete-button\" type=\"submit\">Reject</button>";
        list += "</form>";
        list += "</div>";
        list += "</div>";
    }

    sqlite3_finalize(stmt);

    if (list.empty())
    {
        return "<p class=\"empty\">No follow requests right now.</p>";
    }

    return list;
}

// Draws the rows of a posts query. The query must select:
// post id, body, file_path, created_at, student id, username, photo.
std::string render_posts(sqlite3_stmt* stmt, int viewer_id, bool show_who)
{
    std::string list;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        std::string post_id  = std::to_string(sqlite3_column_int(stmt, 0));
        std::string body     = escape_html(column_text(stmt, 1));
        std::string file     = escape_html(column_text(stmt, 2));
        std::string when     = escape_html(column_text(stmt, 3));
        int author_id        = sqlite3_column_int(stmt, 4);
        std::string author   = escape_html(column_text(stmt, 5));
        std::string photo    = escape_html(column_text(stmt, 6));

        // "2026-09-17 10:57:33" is nicer without the seconds.
        if (when.size() > 16)
        {
            when = when.substr(0, 16);
        }

        list += "<div class=\"post\">";

        list += "<div class=\"post-top\">";

        if (show_who)
        {
            list += "<a class=\"post-who\" href=\"/student/"
                  + std::to_string(author_id) + "\">";
            list += "<img class=\"thumb\" src=\"" + photo + "\" alt=\"\">";
            list += "<span><b>" + author + "</b><br>";
            list += "<span class=\"small\">" + when + "</span></span>";
            list += "</a>";
        }
        else
        {
            list += "<span class=\"small\">" + when + "</span>";
        }

        if (author_id == viewer_id)
        {
            list += "<form action=\"/post/delete\" method=\"post\">";
            list += "<input type=\"hidden\" name=\"id\" value=\"" + post_id + "\">";
            list += "<button class=\"delete-button\" type=\"submit\" "
                    "onclick=\"return confirm('Delete this post?')\">Delete</button>";
            list += "</form>";
        }

        list += "</div>";

        list += "<p class=\"post-body\">" + linkify(body) + "</p>";

        if (!file.empty())
        {
            if (is_pdf(file))
            {
                list += "<a class=\"document-link\" href=\"" + file
                      + "\" target=\"_blank\">";
                list += "<span class=\"pdf-tag\">PDF</span>";
                list += "<span>Open attachment</span>";
                list += "</a>";
            }
            else
            {
                // A screenshot is shown straight away instead of as a link.
                list += "<a href=\"" + file + "\" target=\"_blank\">";
                list += "<img class=\"post-image\" src=\"" + file + "\" alt=\"\">";
                list += "</a>";
            }
        }

        list += "</div>";
    }

    return list;
}

// The posts on somebody's profile.
std::string profile_posts_html(int student_id, int viewer_id)
{
    const char* sql =
        "SELECT p.id, p.body, p.file_path, p.created_at, s.id, s.username, s.photo "
        "FROM posts p JOIN students s ON s.id = p.student_id "
        "WHERE p.student_id = ? ORDER BY p.id DESC;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, student_id);

    std::string list = render_posts(stmt, viewer_id, false);

    sqlite3_finalize(stmt);

    if (list.empty())
    {
        return "<p class=\"empty\">No posts yet.</p>";
    }

    return list;
}

// The "Everyone" tab. Posts from public profiles, plus your own, plus the
// private profiles you have been accepted to follow.
std::string everyone_feed_html(int viewer_id)
{
    const char* sql =
        "SELECT p.id, p.body, p.file_path, p.created_at, s.id, s.username, s.photo "
        "FROM posts p JOIN students s ON s.id = p.student_id "
        "WHERE s.is_private = 0 "
        "   OR s.id = ? "
        "   OR EXISTS (SELECT 1 FROM follows f "
        "               WHERE f.follower_id = ? AND f.following_id = s.id "
        "                 AND f.status = 'accepted') "
        "ORDER BY p.id DESC LIMIT 20;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, viewer_id);
    sqlite3_bind_int(stmt, 2, viewer_id);

    std::string list = render_posts(stmt, viewer_id, true);

    sqlite3_finalize(stmt);

    if (list.empty())
    {
        return "<p class=\"empty\">No posts yet. Be the first to write one.</p>";
    }

    return list;
}

// The "Following" tab. Only the people you follow.
std::string following_feed_html(int viewer_id)
{
    if (viewer_id == 0)
    {
        return "<p class=\"empty\">Log in to see posts from people you follow.</p>";
    }

    const char* sql =
        "SELECT p.id, p.body, p.file_path, p.created_at, s.id, s.username, s.photo "
        "FROM posts p "
        "JOIN students s ON s.id = p.student_id "
        "JOIN follows f ON f.following_id = p.student_id "
        "WHERE f.follower_id = ? AND f.status = 'accepted' "
        "ORDER BY p.id DESC LIMIT 20;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, viewer_id);

    std::string list = render_posts(stmt, viewer_id, true);

    sqlite3_finalize(stmt);

    if (list.empty())
    {
        return "<p class=\"empty\">Nothing here yet. Follow some students and "
               "their posts will show up.</p>";
    }

    return list;
}

// The two tabs above the feed.
std::string tabs_html(bool following)
{
    std::string tabs;

    tabs += "<div class=\"tabs\">";
    tabs += "<a class=\"tab";
    tabs += following ? "" : " active";
    tabs += "\" href=\"/\">Everyone</a>";
    tabs += "<a class=\"tab";
    tabs += following ? " active" : "";
    tabs += "\" href=\"/?tab=following\">Following</a>";
    tabs += "</div>";

    return tabs;
}

// The box at the top of the home page for writing a post.
std::string composer_html(int viewer_id)
{
    if (viewer_id == 0)
    {
        return "";
    }

    std::string box;

    box += "<div class=\"panel composer\">";
    box += "<form action=\"/post\" method=\"post\" "
           "enctype=\"multipart/form-data\">";
    box += "<textarea name=\"body\" rows=\"3\" "
           "placeholder=\"What are you working on? You can paste a link too.\" "
           "required></textarea>";
    box += "<div class=\"composer-foot\">";
    box += "<input type=\"file\" name=\"file\" accept=\".pdf,.jpg,.jpeg,.png\">";
    box += "<button type=\"submit\">Post</button>";
    box += "</div>";
    box += "</form>";
    box += "</div>";

    return box;
}

// Compact rows of students for the sidebar. Much quieter than the big cards.
std::string render_people_rows(sqlite3_stmt* stmt, int viewer_id)
{
    std::string rows;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int student_id       = sqlite3_column_int(stmt, 0);
        std::string id       = std::to_string(student_id);
        std::string username = escape_html(column_text(stmt, 1));
        std::string photo    = escape_html(column_text(stmt, 2));
        std::string program  = escape_html(column_text(stmt, 4));
        int is_private       = sqlite3_column_int(stmt, 5);

        bool show_details = !is_private
                         || viewer_id == student_id
                         || follow_status(viewer_id, student_id) == "accepted";

        rows += "<a class=\"person\" href=\"/student/" + id + "\">";
        rows += "<img class=\"thumb\" src=\"" + photo + "\" alt=\"\">";
        rows += "<span><b>" + username + "</b><br>";
        rows += "<span class=\"small\">";
        rows += show_details ? program : "Private profile";
        rows += "</span></span>";
        rows += "</a>";
    }

    return rows;
}

// The whole right hand column: who to follow, and who is new.
std::string sidebar_html(int viewer_id)
{
    std::string out;

    // Students on your program, if we know it.
    Student me;

    if (viewer_id != 0 && get_student(viewer_id, me) && !me.program.empty())
    {
        const char* sql =
            "SELECT id, username, photo, year, program, is_private FROM students "
            "WHERE program = ? AND id != ? AND is_private = 0 LIMIT 5;";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, me.program.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, viewer_id);

        std::string rows = render_people_rows(stmt, viewer_id);
        sqlite3_finalize(stmt);

        if (!rows.empty())
        {
            out += "<div class=\"panel side\">";
            out += "<h3>Also studying " + escape_html(me.program) + "</h3>";
            out += rows;
            out += "</div>";
        }
    }

    const char* sql =
        "SELECT id, username, photo, year, program, is_private FROM students "
        "ORDER BY id DESC LIMIT 6;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    std::string rows = render_people_rows(stmt, viewer_id);
    sqlite3_finalize(stmt);

    if (!rows.empty())
    {
        out += "<div class=\"panel side\">";
        out += "<h3>New students</h3>";
        out += rows;
        out += "<a class=\"see-all\" href=\"/search\">Search all students</a>";
        out += "</div>";
    }

    return out;
}

std::string chip_row_html(const Student& student)
{
    std::string chips;

    if (!student.program.empty())
    {
        chips += "<span class=\"chip\">" + escape_html(student.program) + "</span>";
    }

    if (!student.year.empty())
    {
        chips += "<span class=\"chip\">" + escape_html(student.year) + "</span>";
    }

    if (!student.marks.empty())
    {
        chips += "<span class=\"chip\">" + escape_html(student.marks) + "</span>";
    }

    return chips;
}

std::string documents_html(int student_id, bool is_owner)
{
    const char* sql =
        "SELECT id, title, file_path FROM documents WHERE student_id = ? "
        "ORDER BY id;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, student_id);

    std::string list;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        std::string id    = std::to_string(sqlite3_column_int(stmt, 0));
        std::string title = escape_html(column_text(stmt, 1));
        std::string path  = escape_html(column_text(stmt, 2));

        list += "<div class=\"document\">";
        list += "<a class=\"document-link\" href=\"" + path
              + "\" target=\"_blank\">";
        std::string kind = is_pdf(path) ? "PDF" : "IMG";
        list += "<span class=\"pdf-tag\">" + kind + "</span>";
        list += "<span>" + title + "</span>";
        list += "</a>";

        // The delete button is only drawn on your own profile.
        if (is_owner)
        {
            list += "<form action=\"/document/delete\" method=\"post\">";
            list += "<input type=\"hidden\" name=\"id\" value=\"" + id + "\">";
            list += "<button class=\"delete-button\" type=\"submit\" "
                    "onclick=\"return confirm('Delete this document?')\">"
                    "Delete</button>";
            list += "</form>";
        }

        list += "</div>";
    }

    sqlite3_finalize(stmt);

    if (list.empty())
    {
        return "<p class=\"empty\">No documents added yet.</p>";
    }

    return list;
}

// The upload form only appears when you are looking at your own profile.
std::string owner_section_html(bool is_owner,
                               const Student& owner,
                               const std::string& message)
{
    if (!is_owner)
    {
        return "";
    }

    std::string form;

    form += "<div class=\"panel\">";
    form += "<h3>Follow requests</h3>";
    form += follow_requests_html(owner.id);
    form += "</div>";

    form += "<div class=\"panel\">";
    form += "<h3>Your profile</h3>";
    form += "<p class=\"muted\">Only you can see these buttons. Your profile is ";
    form += owner.is_private ? "<b>private</b>. " : "<b>public</b>. ";
    form += owner.dm_open
        ? "Anybody can message you."
        : "Only your followers can message you.";
    form += "</p>";
    form += "<a class=\"button-link\" href=\"/edit\">Edit profile</a>";
    form += "</div>";

    form += "<div class=\"panel\">";
    form += "<h3>Add a document</h3>";

    if (!message.empty())
    {
        form += "<p class=\"message\">" + escape_html(message) + "</p>";
    }

    form += "<form action=\"/document\" method=\"post\" "
            "enctype=\"multipart/form-data\">";
    form += "<label>What is it?</label>";
    form += "<input type=\"text\" name=\"title\" placeholder=\"Semester 3 Marksheet\" required>";
    form += "<label>File (PDF, JPG or PNG)</label>";
    form += "<input type=\"file\" name=\"file\" accept=\".pdf,.jpg,.jpeg,.png\" required>";
    form += "<button type=\"submit\">Upload</button>";
    form += "</form>";
    form += "</div>";

    return form;
}

// ---------- whole pages ----------

crow::response form_page(const std::string& file,
                         int user_id,
                         const std::string& message)
{
    std::string html = read_file(file);

    html = replace_all(html, "{{NAV}}", nav_html(user_id));
    html = replace_all(html, "{{MESSAGE}}", escape_html(message));

    return html_page(html);
}

crow::response edit_page(const Student& student, const std::string& message)
{
    std::string html = read_file("templates/edit.html");

    html = replace_all(html, "{{NAV}}", nav_html(student.id));
    html = replace_all(html, "{{MESSAGE}}", escape_html(message));
    html = replace_all(html, "{{USERNAME}}", escape_html(student.username));
    html = replace_all(html, "{{PHOTO}}", escape_html(student.photo));
    html = replace_all(html, "{{BIO}}", escape_html(student.bio));
    html = replace_all(html, "{{YEAR}}", escape_html(student.year));
    html = replace_all(html, "{{PROGRAM}}", escape_html(student.program));
    html = replace_all(html, "{{MARKS}}", escape_html(student.marks));
    html = replace_all(html, "{{PRIVATE_CHECKED}}",
                       student.is_private ? "checked" : "");
    html = replace_all(html, "{{DM_CLOSED_CHECKED}}",
                       student.dm_open ? "" : "checked");

    return html_page(html);
}

crow::response profile_page(const Student& student,
                            int user_id,
                            const std::string& message)
{
    bool is_owner = student.id == user_id;
    bool show_details = can_see_details(student, user_id);

    std::string html = read_file("templates/profile.html");

    html = replace_all(html, "{{NAV}}", nav_html(user_id));
    html = replace_all(html, "{{USERNAME}}", escape_html(student.username));
    html = replace_all(html, "{{PHOTO}}", escape_html(student.photo));
    std::string buttons = follow_button_html(student, user_id);

    if (can_message(user_id, student))
    {
        buttons += "<a class=\"button-link quiet-link\" href=\"/messages/"
                 + std::to_string(student.id) + "\">Message</a>";
    }
    else if (user_id != 0 && user_id != student.id)
    {
        buttons += "<p class=\"small\">" + escape_html(student.username)
                 + " only accepts messages from their followers.</p>";
    }

    html = replace_all(html, "{{FOLLOW_BUTTON}}",
                       "<div class=\"button-row\">" + buttons + "</div>");

    std::string counts;
    counts += "<span class=\"chip\">" + std::to_string(count_followers(student.id))
            + " followers</span>";
    counts += "<span class=\"chip\">" + std::to_string(count_following(student.id))
            + " following</span>";
    html = replace_all(html, "{{COUNTS}}", counts);

    if (student.is_private)
    {
        html = replace_all(html, "{{PRIVATE_BADGE}}",
                           "<p class=\"private-badge\">Private profile</p>");
    }
    else
    {
        html = replace_all(html, "{{PRIVATE_BADGE}}", "");
    }

    // Everything below here is only filled in when the visitor is allowed
    // to see the details. Otherwise they get the locked message instead.
    if (show_details)
    {
        std::string bio = student.bio.empty()
            ? ""
            : "<p class=\"bio\">" + escape_html(student.bio) + "</p>";

        std::string documents =
            "<div class=\"panel\"><h3>Posts</h3>"
            + profile_posts_html(student.id, user_id) + "</div>"
            "<div class=\"panel\"><h3>Documents</h3>"
            + documents_html(student.id, is_owner) + "</div>";

        html = replace_all(html, "{{CHIPS}}", chip_row_html(student));
        html = replace_all(html, "{{BIO}}", bio);
        html = replace_all(html, "{{DETAILS}}", documents);
    }
    else
    {
        std::string locked;
        locked += "<div class=\"panel locked\">";
        locked += "<h3>This profile is private</h3>";
        locked += "<p class=\"muted\">Follow " + escape_html(student.username)
                + " to see their year, program, marks, bio, posts and documents. "
                  "They have to accept your request first.</p>";
        locked += "</div>";

        html = replace_all(html, "{{CHIPS}}", "");
        html = replace_all(html, "{{BIO}}", "");
        html = replace_all(html, "{{DETAILS}}", locked);
    }

    html = replace_all(html, "{{OWNER_SECTION}}",
                       owner_section_html(is_owner, student, message));

    return html_page(html);
}

// ---------- main ----------

int main()
{
    if (sodium_init() < 0)
    {
        std::cerr << "Could not start the security library.\n";
        return 1;
    }

    if (sqlite3_open("data/students.db", &db) != SQLITE_OK)
    {
        std::cerr << "Could not open the database.\n";
        return 1;
    }

    std::string schema = read_file("database/schema.sql");

    if (sqlite3_exec(db, schema.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        std::cerr << "Could not create the tables.\n";
        return 1;
    }

    crow::SimpleApp app;

    CROW_ROUTE(app, "/")
    ([](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        const char* tab = req.url_params.get("tab");
        bool following = tab && std::string(tab) == "following";

        std::string html = read_file("templates/home.html");

        html = replace_all(html, "{{NAV}}", nav_html(user_id));
        html = replace_all(html, "{{COMPOSER}}", composer_html(user_id));
        html = replace_all(html, "{{TABS}}", tabs_html(following));
        html = replace_all(html, "{{FEED}}",
                           following ? following_feed_html(user_id)
                                     : everyone_feed_html(user_id));
        html = replace_all(html, "{{SIDEBAR}}", sidebar_html(user_id));

        // The big welcome banner is only for visitors who are not logged in.
        html = replace_all(html, "{{HERO}}",
            user_id == 0 ? read_file("templates/hero.html") : "");

        return html_page(html);
    });

    CROW_ROUTE(app, "/signup")
    ([](const crow::request& req)
    {
        return form_page("templates/signup.html", logged_in_id(req), "");
    });

    CROW_ROUTE(app, "/signup").methods(crow::HTTPMethod::POST)
    ([](const crow::request& req)
    {
        crow::multipart::message form(req);

        Student student;
        student.username = form.get_part_by_name("username").body;
        student.bio      = form.get_part_by_name("bio").body;
        student.year     = form.get_part_by_name("year").body;
        student.program  = form.get_part_by_name("program").body;
        student.marks    = form.get_part_by_name("marks").body;

        std::string password = form.get_part_by_name("password").body;

        if (!is_valid_username(student.username))
        {
            return form_page("templates/signup.html", 0,
                             "Username can only use letters, numbers and _ .");
        }

        if (password.size() < 6)
        {
            return form_page("templates/signup.html", 0,
                             "Please use a password of at least 6 characters.");
        }

        if (username_taken(student.username))
        {
            return form_page("templates/signup.html", 0,
                             "That username is already taken.");
        }

        crow::multipart::part picture = form.get_part_by_name("photo");
        std::string extension = picture_extension(uploaded_file_name(picture));

        if (picture.body.empty() || extension.empty())
        {
            return form_page("templates/signup.html", 0,
                             "Please choose a .jpg or .png profile picture.");
        }

        if (upload_too_big(picture))
        {
            return form_page("templates/signup.html", 0,
                             "That picture is too big. The limit is 5 MB.");
        }

        student.photo = save_upload(picture,
                                    photo_file_name(student.username, extension));

        if (!add_student(student, hash_password(password)))
        {
            return form_page("templates/signup.html", 0,
                             "Could not save your profile. Please try again.");
        }

        // A document on the signup form is optional.
        int new_id = static_cast<int>(sqlite3_last_insert_rowid(db));

        save_document(new_id,
                         student.username,
                         form.get_part_by_name("doc_title").body,
                         form.get_part_by_name("doc_file"));

        return redirect_to("/login");
    });

    CROW_ROUTE(app, "/login")
    ([](const crow::request& req)
    {
        return form_page("templates/login.html", logged_in_id(req), "");
    });

    CROW_ROUTE(app, "/login").methods(crow::HTTPMethod::POST)
    ([](const crow::request& req)
    {
        crow::query_string form("?" + req.body);

        const char* username = form.get("username");
        const char* password = form.get("password");

        if (!username || !password)
        {
            return form_page("templates/login.html", 0, "Please fill in both fields.");
        }

        int id = check_login(username, password);

        if (id == 0)
        {
            return form_page("templates/login.html", 0, "Wrong username or password.");
        }

        // HttpOnly keeps JavaScript away from the cookie, and SameSite stops
        // another website making your browser send it.
        std::string token = make_session_token();
        create_session(token, id);

        crow::response res = redirect_to("/profile");
        res.set_header("Set-Cookie",
                       "session=" + token + "; Path=/; HttpOnly; SameSite=Strict");
        return res;
    });

    CROW_ROUTE(app, "/logout")
    ([](const crow::request& req)
    {
        // The row is removed as well, so an old cookie is useless afterwards.
        delete_session(get_cookie(req, "session"));

        crow::response res = redirect_to("/login");
        res.set_header("Set-Cookie",
                       "session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        return res;
    });

    CROW_ROUTE(app, "/profile")
    ([](const crow::request& req)
    {
        int user_id = logged_in_id(req);
        Student student;

        if (user_id == 0 || !get_student(user_id, student))
        {
            return redirect_to("/login");
        }

        const char* note = req.url_params.get("note");

        return profile_page(student, user_id, note ? note : "");
    });

    CROW_ROUTE(app, "/edit")
    ([](const crow::request& req)
    {
        int user_id = logged_in_id(req);
        Student student;

        if (user_id == 0 || !get_student(user_id, student))
        {
            return redirect_to("/login");
        }

        return edit_page(student, "");
    });

    CROW_ROUTE(app, "/edit").methods(crow::HTTPMethod::POST)
    ([](const crow::request& req)
    {
        int user_id = logged_in_id(req);
        Student student;

        if (user_id == 0 || !get_student(user_id, student))
        {
            return redirect_to("/login");
        }

        crow::multipart::message form(req);

        std::string old_photo = student.photo;

        student.bio     = form.get_part_by_name("bio").body;
        student.year    = form.get_part_by_name("year").body;
        student.program = form.get_part_by_name("program").body;
        student.marks   = form.get_part_by_name("marks").body;

        // A checkbox only appears in the form when it is ticked.
        student.is_private = form.get_part_by_name("is_private").body.empty() ? 0 : 1;

        // This checkbox is the other way round: ticking it closes your inbox.
        student.dm_open = form.get_part_by_name("dm_closed").body.empty() ? 1 : 0;

        // Choosing a new picture is optional. An empty box keeps the old one.
        crow::multipart::part picture = form.get_part_by_name("photo");

        if (!picture.body.empty())
        {
            std::string extension = picture_extension(uploaded_file_name(picture));

            if (extension.empty())
            {
                return edit_page(student, "A picture must be a .jpg or .png file.");
            }

            if (upload_too_big(picture))
            {
                return edit_page(student, "That picture is too big. The limit is 5 MB.");
            }

            student.photo = save_upload(picture,
                                        photo_file_name(student.username, extension));

            // The old picture is not needed any more.
            std::remove(("." + old_photo).c_str());
        }

        update_student(student);

        return redirect_to("/profile");
    });

    CROW_ROUTE(app, "/document").methods(crow::HTTPMethod::POST)
    ([](const crow::request& req)
    {
        int user_id = logged_in_id(req);
        Student student;

        if (user_id == 0 || !get_student(user_id, student))
        {
            return redirect_to("/login");
        }

        crow::multipart::message form(req);

        bool saved = save_document(user_id,
                                      student.username,
                                      form.get_part_by_name("title").body,
                                      form.get_part_by_name("file"));

        if (!saved)
        {
            return redirect_to("/profile?note=Please+add+a+name+and+a+PDF+file.");
        }

        return redirect_to("/profile");
    });

    // Reads the "id" field that every follow form sends.
    auto posted_id = [](const crow::request& req)
    {
        crow::query_string form("?" + req.body);
        const char* id_text = form.get("id");
        return id_text ? std::atoi(id_text) : 0;
    };

    CROW_ROUTE(app, "/follow").methods(crow::HTTPMethod::POST)
    ([posted_id](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        int target_id = posted_id(req);
        Student target;

        // You cannot follow yourself or somebody who does not exist.
        if (target_id == user_id || !get_student(target_id, target))
        {
            return redirect_to("/search");
        }

        start_follow(user_id, target_id, target.is_private != 0);

        return redirect_to("/student/" + std::to_string(target_id));
    });

    CROW_ROUTE(app, "/unfollow").methods(crow::HTTPMethod::POST)
    ([posted_id](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        int target_id = posted_id(req);
        remove_follow(user_id, target_id);

        return redirect_to("/student/" + std::to_string(target_id));
    });

    CROW_ROUTE(app, "/follow/accept").methods(crow::HTTPMethod::POST)
    ([posted_id](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        // user_id is the one being followed, so only your own requests change.
        accept_follow(posted_id(req), user_id);

        return redirect_to("/profile");
    });

    CROW_ROUTE(app, "/follow/reject").methods(crow::HTTPMethod::POST)
    ([posted_id](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        remove_follow(posted_id(req), user_id);

        return redirect_to("/profile");
    });

    CROW_ROUTE(app, "/post").methods(crow::HTTPMethod::POST)
    ([](const crow::request& req)
    {
        int user_id = logged_in_id(req);
        Student student;

        if (user_id == 0 || !get_student(user_id, student))
        {
            return redirect_to("/login");
        }

        crow::multipart::message form(req);

        add_post(user_id,
                 student.username,
                 form.get_part_by_name("body").body,
                 form.get_part_by_name("file"));

        return redirect_to("/");
    });

    CROW_ROUTE(app, "/post/delete").methods(crow::HTTPMethod::POST)
    ([posted_id](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        std::string file_path;

        // Only your own post can be found here, so only your own can go.
        if (find_own_post(posted_id(req), user_id, file_path))
        {
            if (!file_path.empty())
            {
                std::remove(("." + file_path).c_str());
            }

            delete_post(posted_id(req), user_id);
        }

        return redirect_to("/profile");
    });

    CROW_ROUTE(app, "/messages")
    ([](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        std::string html = read_file("templates/inbox.html");

        html = replace_all(html, "{{NAV}}", nav_html(user_id));
        html = replace_all(html, "{{INBOX}}", inbox_html(user_id));

        return html_page(html);
    });

    CROW_ROUTE(app, "/messages/<int>")
    ([](const crow::request& req, int other_id)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        Student other;

        if (!get_student(other_id, other) || other_id == user_id)
        {
            return redirect_to("/messages");
        }

        std::string html = read_file("templates/chat.html");

        html = replace_all(html, "{{NAV}}", nav_html(user_id));
        html = replace_all(html, "{{OTHER_ID}}", std::to_string(other_id));
        html = replace_all(html, "{{OTHER_NAME}}", escape_html(other.username));
        html = replace_all(html, "{{OTHER_PHOTO}}", escape_html(other.photo));
        html = replace_all(html, "{{MESSAGES}}", conversation_html(user_id, other_id));

        // The box to type in is only shown if you are allowed to write.
        if (can_message(user_id, other))
        {
            std::string box;
            box += "<form class=\"search-bar\" action=\"/messages/send\" "
                   "method=\"post\">";
            box += "<input type=\"hidden\" name=\"id\" value=\""
                 + std::to_string(other_id) + "\">";
            box += "<input type=\"text\" name=\"body\" "
                   "placeholder=\"Write a message\" required>";
            box += "<button type=\"submit\">Send</button>";
            box += "</form>";
            html = replace_all(html, "{{SEND_BOX}}", box);
        }
        else
        {
            html = replace_all(html, "{{SEND_BOX}}",
                "<p class=\"empty\">" + escape_html(other.username)
                + " only accepts messages from their followers.</p>");
        }

        return html_page(html);
    });

    CROW_ROUTE(app, "/messages/send").methods(crow::HTTPMethod::POST)
    ([](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        crow::query_string form("?" + req.body);
        const char* id_text = form.get("id");
        const char* body    = form.get("body");

        if (!id_text || !body)
        {
            return redirect_to("/messages");
        }

        int other_id = std::atoi(id_text);
        Student other;

        if (!get_student(other_id, other))
        {
            return redirect_to("/messages");
        }

        // The rule is checked again here, not just when drawing the page.
        if (can_message(user_id, other)
            && !std::string(body).empty()
            && std::string(body).size() <= 2000)
        {
            add_message(user_id, other_id, body);
        }

        return redirect_to("/messages/" + std::to_string(other_id));
    });

    CROW_ROUTE(app, "/document/delete").methods(crow::HTTPMethod::POST)
    ([](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        crow::query_string form("?" + req.body);
        const char* id_text = form.get("id");

        if (!id_text)
        {
            return redirect_to("/profile");
        }

        int document_id = std::atoi(id_text);
        std::string file_path;

        // Nothing happens unless the document is really yours.
        if (find_own_document(document_id, user_id, file_path))
        {
            std::remove(("." + file_path).c_str());
            delete_document(document_id, user_id);
        }

        return redirect_to("/profile");
    });

    CROW_ROUTE(app, "/student/<int>")
    ([](const crow::request& req, int id)
    {
        Student student;

        if (!get_student(id, student))
        {
            return crow::response(404, "Student not found.");
        }

        return profile_page(student, logged_in_id(req), "");
    });

    CROW_ROUTE(app, "/search")
    ([](const crow::request& req)
    {
        const char* q = req.url_params.get("q");
        std::string query = q ? q : "";

        const char* sql =
            "SELECT id, username, photo, year, program, is_private FROM students "
            "WHERE username LIKE ? OR (program LIKE ? AND is_private = 0) "
            "ORDER BY username;";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

        std::string pattern = "%" + query + "%";
        sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);

        std::string cards = render_cards(stmt, logged_in_id(req));
        sqlite3_finalize(stmt);

        std::string results = cards.empty()
            ? "<p class=\"empty\">No students found.</p>"
            : "<div class=\"grid\">" + cards + "</div>";

        std::string html = read_file("templates/search.html");

        html = replace_all(html, "{{NAV}}", nav_html(logged_in_id(req)));
        html = replace_all(html, "{{QUERY}}", escape_html(query));
        html = replace_all(html, "{{RESULTS}}", results);

        return html_page(html);
    });

    std::cout << "Open http://localhost:8090 in your browser.\n";

    app.port(8090).multithreaded().run();

    sqlite3_close(db);
    return 0;
}
