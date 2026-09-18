#include <crow.h>
#include <sqlite3.h>
#include <sodium.h>
#include <curl/curl.h>

#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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

// A name no other upload can have. The time makes the folder easy to read,
// and the random part means two files saved in the same second still get
// different names instead of one quietly replacing the other.
std::string unique_file_name(const std::string& username,
                             const std::string& kind,
                             const std::string& extension)
{
    unsigned char bytes[4];
    char text[9];

    randombytes_buf(bytes, sizeof bytes);
    sodium_bin2hex(text, sizeof text, bytes, sizeof bytes);

    return username + "_" + kind + std::to_string(std::time(nullptr))
         + "_" + std::string(text) + extension;
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

// A login older than this stops working and the student signs in again.
const int SESSION_DAYS = 30;

// Old rows are no use to anybody, so clear them out whenever somebody logs in.
void remove_stale_sessions()
{
    const char* sql =
        "DELETE FROM sessions "
        "WHERE created_at <= datetime('now', '-' || ? || ' days');";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, SESSION_DAYS);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void create_session(const std::string& token, int student_id)
{
    remove_stale_sessions();

    const char* sql =
        "INSERT INTO sessions(token, student_id, created_at) "
        "VALUES(?, ?, datetime('now'));";

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

    const char* sql =
        "SELECT student_id FROM sessions "
        "WHERE token = ? AND created_at > datetime('now', '-' || ? || ' days');";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, SESSION_DAYS);

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

    std::string saved_name = unique_file_name(username, "doc", extension);

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

// ---------- link previews ----------

// Finds the first web address in a piece of text.
std::string first_url(const std::string& text)
{
    size_t start = text.find("http://");
    size_t https = text.find("https://");

    if (https != std::string::npos && (start == std::string::npos || https < start))
    {
        start = https;
    }

    if (start == std::string::npos)
    {
        return "";
    }

    size_t end = start;

    while (end < text.size()
           && text[end] != ' '
           && text[end] != '\n'
           && text[end] != '\r'
           && text[end] != '\t')
    {
        end++;
    }

    std::string url = text.substr(start, end - start);

    // A full stop or bracket at the very end is almost always punctuation.
    while (!url.empty() && (url.back() == '.' || url.back() == ','
                            || url.back() == ')' || url.back() == '!'))
    {
        url.pop_back();
    }

    return url.size() > 500 ? "" : url;
}

// True for addresses inside this computer or the local network. Fetching one
// of those would let a student make the server read things that are not meant
// to be public, so they are refused.
bool is_private_ipv4(const std::string& ip)
{
    int a = 0, b = 0;

    if (std::sscanf(ip.c_str(), "%d.%d", &a, &b) != 2)
    {
        return true;
    }

    if (a == 0 || a == 10 || a == 127) return true;
    if (a == 169 && b == 254) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 100 && b >= 64 && b <= 127) return true;

    return false;
}

bool is_private_address(const std::string& address)
{
    if (address.empty())
    {
        return true;
    }

    std::string ip;

    for (char c : address)
    {
        ip += std::tolower(c);
    }

    // An IPv6 address. Plain IPv4 rules do not apply to it.
    if (ip.find(':') != std::string::npos)
    {
        if (ip == "::1" || ip == "::")
        {
            return true;
        }

        // Unique local (fc00::/7) and link local (fe80::/10).
        if (ip.rfind("fc", 0) == 0 || ip.rfind("fd", 0) == 0
            || ip.rfind("fe80", 0) == 0)
        {
            return true;
        }

        // An IPv4 address written inside an IPv6 one, like ::ffff:127.0.0.1,
        // has to be checked with the IPv4 rules or it would slip through.
        size_t last_colon = ip.rfind(':');

        if (ip.find('.') != std::string::npos)
        {
            return is_private_ipv4(ip.substr(last_colon + 1));
        }

        return false;
    }

    return is_private_ipv4(ip);
}

// curl calls this once it knows the address it connected to, before asking
// for the page. Returning the abort value stops the request there.
int check_address(void*, char* primary_ip, char*, int, int)
{
    return is_private_address(primary_ip ? primary_ip : "")
        ? CURL_PREREQFUNC_ABORT
        : CURL_PREREQFUNC_OK;
}

const size_t MAX_PAGE_BYTES = 256 * 1024;

size_t collect_page(char* data, size_t size, size_t count, void* target)
{
    std::string* page = static_cast<std::string*>(target);
    size_t bytes = size * count;

    if (page->size() + bytes > MAX_PAGE_BYTES)
    {
        return 0;
    }

    page->append(data, bytes);
    return bytes;
}

// Downloads the start of a page. Everything here is deliberately limited:
// http and https only, a few seconds, a few redirects, a small download,
// and no addresses on this machine or the local network.
std::string fetch_page(const std::string& url)
{
    CURL* curl = curl_easy_init();

    if (!curl)
    {
        return "";
    }

    std::string page;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, check_address);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_page);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &page);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "StudentProfiles/1.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return page;
}

struct LinkPreview
{
    std::string url;
    std::string title;
    std::string description;
    std::string image;
    std::string site;
};

std::string lowercase_copy(std::string text)
{
    for (char& c : text)
    {
        c = std::tolower(c);
    }

    return text;
}

// Turns the few HTML codes that show up in page titles back into characters.
std::string undo_html_codes(std::string text)
{
    text = replace_all(text, "&amp;", "&");
    text = replace_all(text, "&quot;", "\"");
    text = replace_all(text, "&#39;", "'");
    text = replace_all(text, "&apos;", "'");
    text = replace_all(text, "&lt;", "<");
    text = replace_all(text, "&gt;", ">");
    text = replace_all(text, "&nbsp;", " ");
    return text;
}

// Pulls one <meta> value out of the page, for example og:title.
std::string read_meta(const std::string& page, const std::string& name)
{
    std::string lower = lowercase_copy(page);
    size_t at = lower.find("\"" + name + "\"");

    if (at == std::string::npos)
    {
        return "";
    }

    // The content attribute can sit before or after the name attribute, so
    // look from the start of this tag.
    size_t tag_start = lower.rfind("<meta", at);
    size_t tag_end = lower.find('>', at);

    if (tag_start == std::string::npos || tag_end == std::string::npos)
    {
        return "";
    }

    size_t content = lower.find("content=", tag_start);

    if (content == std::string::npos || content > tag_end)
    {
        return "";
    }

    size_t quote = page.find_first_of("\"'", content);

    if (quote == std::string::npos || quote > tag_end)
    {
        return "";
    }

    size_t close = page.find(page[quote], quote + 1);

    if (close == std::string::npos)
    {
        return "";
    }

    std::string value = page.substr(quote + 1, close - quote - 1);

    return undo_html_codes(value).substr(0, 300);
}

std::string read_title_tag(const std::string& page)
{
    std::string lower = lowercase_copy(page);
    size_t start = lower.find("<title");

    if (start == std::string::npos)
    {
        return "";
    }

    start = lower.find('>', start);
    size_t end = lower.find("</title", start);

    if (start == std::string::npos || end == std::string::npos)
    {
        return "";
    }

    std::string value = page.substr(start + 1, end - start - 1);

    // Tidy up newlines and runs of spaces.
    std::string clean;

    for (char c : value)
    {
        char ch = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;

        if (ch == ' ' && (clean.empty() || clean.back() == ' '))
        {
            continue;
        }

        clean += ch;
    }

    while (!clean.empty() && clean.back() == ' ')
    {
        clean.pop_back();
    }

    return undo_html_codes(clean).substr(0, 300);
}

// The part of the address a person recognises, like github.com
std::string domain_of(const std::string& url)
{
    size_t start = url.find("//");

    if (start == std::string::npos)
    {
        return "";
    }

    start += 2;
    size_t end = url.find('/', start);
    std::string host = url.substr(start, end == std::string::npos
                                         ? std::string::npos
                                         : end - start);

    if (host.rfind("www.", 0) == 0)
    {
        host = host.substr(4);
    }

    return host;
}

void save_link_preview(const LinkPreview& p)
{
    const char* sql =
        "INSERT OR REPLACE INTO link_previews(url, title, description, image, site) "
        "VALUES(?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, p.url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, p.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, p.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, p.image.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, p.site.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool get_link_preview(const std::string& url, LinkPreview& out)
{
    const char* sql =
        "SELECT url, title, description, image, site FROM link_previews "
        "WHERE url = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.url         = column_text(stmt, 0);
        out.title       = column_text(stmt, 1);
        out.description = column_text(stmt, 2);
        out.image       = column_text(stmt, 3);
        out.site        = column_text(stmt, 4);
        found = true;
    }

    sqlite3_finalize(stmt);
    return found;
}

// Fetches a page once and remembers what it said about itself.
void remember_link(const std::string& url)
{
    LinkPreview existing;

    if (url.empty() || get_link_preview(url, existing))
    {
        return;
    }

    std::string page = fetch_page(url);

    LinkPreview p;
    p.url  = url;
    p.site = domain_of(url);

    if (!page.empty())
    {
        p.title = read_meta(page, "og:title");

        if (p.title.empty())
        {
            p.title = read_title_tag(page);
        }

        p.description = read_meta(page, "og:description");

        if (p.description.empty())
        {
            p.description = read_meta(page, "description");
        }

        std::string image = read_meta(page, "og:image");

        // Only a normal web address is kept, never anything else.
        if (image.rfind("http://", 0) == 0 || image.rfind("https://", 0) == 0)
        {
            p.image = image;
        }
    }

    // The row is saved even when nothing was found, so the same address is
    // not fetched again on every visit.
    save_link_preview(p);
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

        web_path = save_upload(file, unique_file_name(username, "post", extension));
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

    // Look the link up once, now, so showing the post later is just a read.
    remember_link(first_url(body));

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

// ---------- likes and comments ----------

int count_likes(int post_id)
{
    const char* sql = "SELECT COUNT(*) FROM likes WHERE post_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, post_id);

    int total = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        total = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return total;
}

int count_comments(int post_id)
{
    const char* sql = "SELECT COUNT(*) FROM comments WHERE post_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, post_id);

    int total = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        total = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return total;
}

bool viewer_likes(int post_id, int student_id)
{
    if (student_id == 0)
    {
        return false;
    }

    const char* sql =
        "SELECT 1 FROM likes WHERE post_id = ? AND student_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, student_id);

    bool found = sqlite3_step(stmt) == SQLITE_ROW;

    sqlite3_finalize(stmt);
    return found;
}

// Pressing the heart a second time takes the like back.
void toggle_like(int post_id, int student_id)
{
    bool already = viewer_likes(post_id, student_id);

    // The SELECT ... WHERE EXISTS keeps a like for a post that is not there
    // from ever being written.
    const char* sql = already
        ? "DELETE FROM likes WHERE post_id = ? AND student_id = ?;"
        : "INSERT OR IGNORE INTO likes(post_id, student_id) "
          "SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?);";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, student_id);

    if (!already)
    {
        sqlite3_bind_int(stmt, 3, post_id);
    }

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool add_comment(int post_id, int student_id, const std::string& body)
{
    if (body.empty() || body.size() > 500)
    {
        return false;
    }

    const char* sql =
        "INSERT INTO comments(post_id, student_id, body) "
        "SELECT ?, ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?);";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, student_id);
    sqlite3_bind_text(stmt, 3, body.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, post_id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // The insert does nothing when the post is gone, and that is not a
    // comment having been written.
    return sqlite3_changes(db) > 0;
}

// A comment goes when you wrote it yourself, or when the post it sits under
// is yours. Anybody else asking for the same id changes nothing.
void delete_comment(int comment_id, int student_id)
{
    const char* sql =
        "DELETE FROM comments WHERE id = ? AND ("
        "    student_id = ? "
        " OR post_id IN (SELECT id FROM posts WHERE student_id = ?));";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, comment_id);
    sqlite3_bind_int(stmt, 2, student_id);
    sqlite3_bind_int(stmt, 3, student_id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// The likes and comments of a deleted post have nothing left to sit under.
void delete_post_replies(int post_id)
{
    const char* statements[] = {
        "DELETE FROM likes WHERE post_id = ?;",
        "DELETE FROM comments WHERE post_id = ?;"
    };

    for (const char* sql : statements)
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, post_id);

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// ---------- notifications ----------

int post_owner(int post_id)
{
    const char* sql = "SELECT student_id FROM posts WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, post_id);

    int id = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        id = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return id;
}

// Nobody needs telling about their own like or their own comment.
void add_notification(int post_id, int actor_id, const char* kind)
{
    int owner = post_owner(post_id);

    if (owner == 0 || owner == actor_id)
    {
        return;
    }

    const char* sql =
        "INSERT INTO notifications(student_id, actor_id, post_id, kind) "
        "VALUES(?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, owner);
    sqlite3_bind_int(stmt, 2, actor_id);
    sqlite3_bind_int(stmt, 3, post_id);
    sqlite3_bind_text(stmt, 4, kind, -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// Taking a like back takes the "liked your post" line with it, so pressing
// the heart on and off does not pile up notices.
void remove_like_notification(int post_id, int actor_id)
{
    const char* sql =
        "DELETE FROM notifications "
        "WHERE post_id = ? AND actor_id = ? AND kind = 'like';";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, actor_id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int count_unseen(int student_id)
{
    if (student_id == 0)
    {
        return 0;
    }

    const char* sql =
        "SELECT COUNT(*) FROM notifications "
        "WHERE student_id = ? AND seen = 0;";

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

void mark_notifications_seen(int student_id)
{
    const char* sql =
        "UPDATE notifications SET seen = 1 WHERE student_id = ? AND seen = 0;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, student_id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void delete_post_notifications(int post_id)
{
    const char* sql = "DELETE FROM notifications WHERE post_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, post_id);

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

int count_posts(int student_id)
{
    return count_follows(student_id,
        "SELECT COUNT(*) FROM posts WHERE student_id = ?;");
}

int count_pending_requests(int student_id)
{
    return count_follows(student_id,
        "SELECT COUNT(*) FROM follows "
        "WHERE following_id = ? AND status = 'pending';");
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

// The list on the alerts page. Newest first, and a line links straight to
// the post it is about.
std::string notifications_html(int student_id)
{
    const char* sql =
        "SELECT n.kind, n.created_at, n.seen, n.post_id, "
        "       s.id, s.username, s.photo, p.body "
        "FROM notifications n "
        "JOIN students s ON s.id = n.actor_id "
        "JOIN posts p ON p.id = n.post_id "
        "WHERE n.student_id = ? ORDER BY n.id DESC LIMIT 50;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, student_id);

    std::string list;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        std::string kind   = column_text(stmt, 0);
        std::string when   = escape_html(column_text(stmt, 1));
        bool seen          = sqlite3_column_int(stmt, 2) != 0;
        int actor_id       = sqlite3_column_int(stmt, 4);
        std::string actor  = escape_html(column_text(stmt, 5));
        std::string photo  = escape_html(column_text(stmt, 6));
        std::string body   = escape_html(column_text(stmt, 7));

        if (when.size() > 16)
        {
            when = when.substr(0, 16);
        }

        // Just enough of the post to know which one is meant.
        if (body.size() > 60)
        {
            body = body.substr(0, 60) + "...";
        }

        list += "<div class=\"alert-row";
        list += seen ? "" : " unseen";
        list += "\">";

        list += "<a href=\"/student/" + std::to_string(actor_id) + "\">";
        list += "<img class=\"thumb small-thumb\" src=\"" + photo + "\" alt=\"\">";
        list += "</a>";

        list += "<div class=\"alert-text\">";
        list += "<a class=\"comment-name\" href=\"/student/"
              + std::to_string(actor_id) + "\"><b>" + actor + "</b></a> ";
        list += kind == "like" ? "liked your post" : "commented on your post";
        list += "<p class=\"alert-post\">" + body + "</p>";
        list += "<span class=\"small\">" + when + "</span>";
        list += "</div>";

        list += "</div>";
    }

    sqlite3_finalize(stmt);

    if (list.empty())
    {
        return "<p class=\"empty\">Nothing yet. Likes and comments on your "
               "posts show up here.</p>";
    }

    return list;
}

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

        // The count only appears when there is something waiting.
        int waiting = count_unseen(user_id);

        links += "<a href=\"/notifications\">Alerts";

        if (waiting > 0)
        {
            links += "<span class=\"badge\">" + std::to_string(waiting) + "</span>";
        }

        links += "</a>";

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
// Shows an uploaded file instead of only linking to it. A picture is shown as
// a picture. A PDF is shown in the viewer the browser already has, with a
// message inside as a fallback, because some phones cannot show one this way.
// The preview card under a post that contains a link.
std::string link_card_html(const std::string& body)
{
    LinkPreview p;

    if (!get_link_preview(first_url(body), p) || p.title.empty())
    {
        return "";
    }

    std::string url   = escape_html(p.url);
    std::string title = escape_html(p.title);
    std::string desc  = escape_html(p.description);
    std::string site  = escape_html(p.site);
    std::string image = escape_html(p.image);

    std::string card;

    card += "<a class=\"link-card\" href=\"" + url
          + "\" target=\"_blank\" rel=\"noopener noreferrer\">";

    if (!image.empty())
    {
        card += "<img class=\"link-card-image\" src=\"" + image
              + "\" alt=\"\" loading=\"lazy\">";
    }

    card += "<span class=\"link-card-text\">";
    card += "<span class=\"small\">" + site + "</span>";
    card += "<b>" + title + "</b>";

    if (!desc.empty())
    {
        card += "<span class=\"link-card-desc\">" + desc + "</span>";
    }

    card += "</span>";
    card += "</a>";

    return card;
}

std::string attachment_preview(const std::string& path)
{
    if (path.empty())
    {
        return "";
    }

    if (!is_pdf(path))
    {
        return "<a href=\"" + path + "\" target=\"_blank\">"
               "<img class=\"post-image\" src=\"" + path + "\" alt=\"\"></a>";
    }

    std::string out;

    out += "<div class=\"pdf-preview\">";
    out += "<object data=\"" + path + "\" type=\"application/pdf\">";
    out += "<p class=\"pdf-fallback\">This browser cannot show the PDF here. "
           "Use the link below to open it.</p>";
    out += "</object>";
    out += "</div>";

    return out;
}

// Where a like or a comment button should send you back to. It comes from a
// hidden field in the form, so anything that is not a path on this site is
// thrown away rather than followed.
std::string safe_back(const std::string& back)
{
    if (back.size() < 2 || back[0] != '/' || back[1] == '/')
    {
        return "/";
    }

    return back;
}

// The heart, the count, and how many comments there are. The numbers are
// read once by the feed query and handed in, rather than looked up here.
std::string post_actions_html(int post_id,
                              int likes,
                              bool liked,
                              int comments,
                              int viewer_id,
                              const std::string& back)
{
    std::string id = std::to_string(post_id);
    std::string out;

    out += "<div class=\"post-actions\">";

    if (viewer_id == 0)
    {
        // A visitor who is not logged in sees the count but cannot press it.
        out += "<span class=\"like-count\">&#9825; ";
        out += std::to_string(likes);
        out += "</span>";
    }
    else
    {
        out += "<form action=\"/post/like\" method=\"post\">";
        out += "<input type=\"hidden\" name=\"id\" value=\"" + id + "\">";
        out += "<input type=\"hidden\" name=\"back\" value=\""
             + escape_html(back) + "\">";
        out += "<button class=\"like-button";
        out += liked ? " liked" : "";
        out += "\" type=\"submit\">";
        out += liked ? "&#9829; " : "&#9825; ";
        out += std::to_string(likes);
        out += "</button>";
        out += "</form>";
    }

    out += "<span class=\"small\">";
    out += std::to_string(comments);
    out += comments == 1 ? " comment" : " comments";
    out += "</span>";

    out += "</div>";

    return out;
}

// The box for writing a comment, which only a logged in student gets.
std::string comment_box_html(int post_id, int viewer_id, const std::string& back)
{
    if (viewer_id == 0)
    {
        return "";
    }

    std::string out;

    out += "<form class=\"comment-form\" action=\"/comment\" method=\"post\">";
    out += "<input type=\"hidden\" name=\"post_id\" value=\""
         + std::to_string(post_id) + "\">";
    out += "<input type=\"hidden\" name=\"back\" value=\""
         + escape_html(back) + "\">";
    out += "<input type=\"text\" name=\"body\" maxlength=\"500\" "
           "placeholder=\"Write a comment\">";
    out += "<button type=\"submit\">Send</button>";
    out += "</form>";

    return out;
}

// Every comment for a whole page of posts, read in one query instead of one
// query per post, and handed back as ready HTML for each post in turn.
std::map<int, std::string> comments_for_posts(const std::vector<int>& post_ids,
                                              const std::map<int, int>& authors,
                                              int viewer_id,
                                              const std::string& back)
{
    std::map<int, std::string> out;

    if (post_ids.empty())
    {
        return out;
    }

    // One "?" for each post, so the whole page is a single IN (...) lookup.
    std::string sql =
        "SELECT c.post_id, c.id, c.body, c.created_at, s.id, s.username, s.photo "
        "FROM comments c JOIN students s ON s.id = c.student_id "
        "WHERE c.post_id IN (";

    for (size_t i = 0; i < post_ids.size(); ++i)
    {
        sql += i == 0 ? "?" : ",?";
    }

    sql += ") ORDER BY c.id;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    for (size_t i = 0; i < post_ids.size(); ++i)
    {
        sqlite3_bind_int(stmt, static_cast<int>(i) + 1, post_ids[i]);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int post_id            = sqlite3_column_int(stmt, 0);
        std::string comment_id = std::to_string(sqlite3_column_int(stmt, 1));
        std::string body       = escape_html(column_text(stmt, 2));
        std::string when       = escape_html(column_text(stmt, 3));
        int author_id          = sqlite3_column_int(stmt, 4);
        std::string author     = escape_html(column_text(stmt, 5));
        std::string photo      = escape_html(column_text(stmt, 6));

        if (when.size() > 16)
        {
            when = when.substr(0, 16);
        }

        auto owner = authors.find(post_id);
        int post_author_id = owner == authors.end() ? 0 : owner->second;

        std::string& block = out[post_id];

        block += "<div class=\"comment\">";

        block += "<a class=\"comment-who\" href=\"/student/"
               + std::to_string(author_id) + "\">";
        block += "<img class=\"thumb small-thumb\" src=\"" + photo + "\" alt=\"\">";
        block += "</a>";

        block += "<div class=\"comment-text\">";
        block += "<a class=\"comment-name\" href=\"/student/"
               + std::to_string(author_id) + "\"><b>" + author + "</b></a> ";
        block += "<span class=\"small\">" + when + "</span>";
        block += "<p class=\"comment-body\">" + linkify(body) + "</p>";
        block += "</div>";

        // Your own comment is yours to remove, and so is any comment sitting
        // under a post of yours.
        if (viewer_id != 0
            && (author_id == viewer_id || post_author_id == viewer_id))
        {
            block += "<form action=\"/comment/delete\" method=\"post\">";
            block += "<input type=\"hidden\" name=\"id\" value=\"" + comment_id + "\">";
            block += "<input type=\"hidden\" name=\"back\" value=\""
                   + escape_html(back) + "\">";
            block += "<button class=\"delete-button\" type=\"submit\" "
                     "onclick=\"return confirm('Delete this comment?')\">"
                     "Delete</button>";
            block += "</form>";
        }

        block += "</div>";
    }

    sqlite3_finalize(stmt);
    return out;
}

// One post as it was read from the feed query.
struct FeedPost
{
    int id = 0;
    int author_id = 0;
    std::string raw_body;
    std::string body;
    std::string file;
    std::string when;
    std::string author;
    std::string photo;
    int likes = 0;
    bool liked = false;
    int comments = 0;
};

// max_posts of 0 means draw everything the query returned. A profile asks
// for one more row than it shows, so it can tell whether older posts exist,
// and then caps the drawing here.
std::string render_posts(sqlite3_stmt* stmt,
                         int viewer_id,
                         bool show_who,
                         const std::string& back,
                         int max_posts = 0)
{
    // The rows are read first and drawn afterwards, so the comments for the
    // whole page can be fetched in one go rather than one post at a time.
    std::vector<FeedPost> posts;
    std::vector<int> ids;
    std::map<int, int> authors;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        FeedPost post;

        post.id        = sqlite3_column_int(stmt, 0);
        post.raw_body  = column_text(stmt, 1);
        post.body      = escape_html(post.raw_body);
        post.file      = escape_html(column_text(stmt, 2));
        post.when      = escape_html(column_text(stmt, 3));
        post.author_id = sqlite3_column_int(stmt, 4);
        post.author    = escape_html(column_text(stmt, 5));
        post.photo     = escape_html(column_text(stmt, 6));
        post.likes     = sqlite3_column_int(stmt, 7);
        post.liked     = sqlite3_column_int(stmt, 8) != 0;
        post.comments  = sqlite3_column_int(stmt, 9);

        // "2026-09-17 10:57:33" is nicer without the seconds.
        if (post.when.size() > 16)
        {
            post.when = post.when.substr(0, 16);
        }

        ids.push_back(post.id);
        authors[post.id] = post.author_id;
        posts.push_back(post);

        if (max_posts > 0 && static_cast<int>(posts.size()) >= max_posts)
        {
            break;
        }
    }

    std::map<int, std::string> comments =
        comments_for_posts(ids, authors, viewer_id, back);

    std::string list;

    for (const FeedPost& post : posts)
    {
        std::string post_id = std::to_string(post.id);

        list += "<div class=\"post\">";

        list += "<div class=\"post-top\">";

        if (show_who)
        {
            list += "<a class=\"post-who\" href=\"/student/"
                  + std::to_string(post.author_id) + "\">";
            list += "<img class=\"thumb\" src=\"" + post.photo + "\" alt=\"\">";
            list += "<span><b>" + post.author + "</b><br>";
            list += "<span class=\"small\">" + post.when + "</span></span>";
            list += "</a>";
        }
        else
        {
            list += "<span class=\"small\">" + post.when + "</span>";
        }

        if (post.author_id == viewer_id)
        {
            list += "<form action=\"/post/delete\" method=\"post\">";
            list += "<input type=\"hidden\" name=\"id\" value=\"" + post_id + "\">";
            list += "<button class=\"delete-button\" type=\"submit\" "
                    "onclick=\"return confirm('Delete this post?')\">Delete</button>";
            list += "</form>";
        }

        list += "</div>";

        list += "<p class=\"post-body\">" + linkify(post.body) + "</p>";

        list += link_card_html(post.raw_body);

        if (!post.file.empty())
        {
            list += attachment_preview(post.file);

            if (is_pdf(post.file))
            {
                list += "<a class=\"document-link\" href=\"" + post.file
                      + "\" target=\"_blank\">";
                list += "<span class=\"pdf-tag\">PDF</span>";
                list += "<span>Open full size</span>";
                list += "</a>";
            }
        }

        list += post_actions_html(post.id, post.likes, post.liked,
                                  post.comments, viewer_id, back);

        std::string rows;
        auto found = comments.find(post.id);

        if (found != comments.end())
        {
            rows = found->second;
        }

        std::string box = comment_box_html(post.id, viewer_id, back);

        if (!rows.empty() || !box.empty())
        {
            list += "<div class=\"comments\">" + rows + box + "</div>";
        }

        list += "</div>";
    }

    return list;
}

// The posts on somebody's profile.
// How many posts a profile shows before asking you to say so.
const int PROFILE_POSTS = 20;

std::string profile_posts_html(int student_id,
                               int viewer_id,
                               const std::string& back,
                               bool show_all)
{
    const char* sql =
        "SELECT p.id, p.body, p.file_path, p.created_at, s.id, s.username, s.photo, "
        "       (SELECT COUNT(*) FROM likes l WHERE l.post_id = p.id), "
        "       EXISTS(SELECT 1 FROM likes l "
        "               WHERE l.post_id = p.id AND l.student_id = ?), "
        "       (SELECT COUNT(*) FROM comments c WHERE c.post_id = p.id) "
        "FROM posts p JOIN students s ON s.id = p.student_id "
        "WHERE p.student_id = ? ORDER BY p.id DESC LIMIT ?;";

    // One more than the page, purely to find out whether there are older ones.
    int wanted = show_all ? -1 : PROFILE_POSTS + 1;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, viewer_id);
    sqlite3_bind_int(stmt, 2, student_id);
    sqlite3_bind_int(stmt, 3, wanted);

    std::string list = render_posts(stmt, viewer_id, false, back,
                                    show_all ? 0 : PROFILE_POSTS);

    sqlite3_finalize(stmt);

    if (list.empty())
    {
        return "<p class=\"empty\">No posts yet.</p>";
    }

    if (!show_all && count_posts(student_id) > PROFILE_POSTS)
    {
        list += "<a class=\"see-all\" href=\"" + escape_html(back)
              + "?posts=all\">Show older posts</a>";
    }

    return list;
}

// The "Everyone" tab. Posts from public profiles, plus your own, plus the
// private profiles you have been accepted to follow.
std::string everyone_feed_html(int viewer_id)
{
    const char* sql =
        "SELECT p.id, p.body, p.file_path, p.created_at, s.id, s.username, s.photo, "
        "       (SELECT COUNT(*) FROM likes l WHERE l.post_id = p.id), "
        "       EXISTS(SELECT 1 FROM likes l "
        "               WHERE l.post_id = p.id AND l.student_id = ?), "
        "       (SELECT COUNT(*) FROM comments c WHERE c.post_id = p.id) "
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
    sqlite3_bind_int(stmt, 3, viewer_id);

    std::string list = render_posts(stmt, viewer_id, true, "/");

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
        "SELECT p.id, p.body, p.file_path, p.created_at, s.id, s.username, s.photo, "
        "       (SELECT COUNT(*) FROM likes l WHERE l.post_id = p.id), "
        "       EXISTS(SELECT 1 FROM likes l "
        "               WHERE l.post_id = p.id AND l.student_id = ?), "
        "       (SELECT COUNT(*) FROM comments c WHERE c.post_id = p.id) "
        "FROM posts p "
        "JOIN students s ON s.id = p.student_id "
        "JOIN follows f ON f.following_id = p.student_id "
        "WHERE f.follower_id = ? AND f.status = 'accepted' "
        "ORDER BY p.id DESC LIMIT 20;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, viewer_id);
    sqlite3_bind_int(stmt, 2, viewer_id);

    std::string list = render_posts(stmt, viewer_id, true, "/?tab=following");

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

        // The title and the delete button sit on top, the file itself below.
        list += "<div class=\"document-card\">";
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
        list += attachment_preview(path);
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
// The upload box, shown inside the Documents tab on your own profile.
std::string add_document_html(const std::string& message)
{
    std::string form;

    form += "<div class=\"panel add-box\">";
    form += "<h3>Add a document</h3>";
    form += "<p class=\"muted small-note\">A marksheet, a certificate, a "
            "transcript - anything you want on your profile.</p>";

    if (!message.empty())
    {
        form += "<p class=\"message\">" + escape_html(message) + "</p>";
    }

    form += "<form action=\"/document\" method=\"post\" "
            "enctype=\"multipart/form-data\">";
    form += "<label>What is it?</label>";
    form += "<input type=\"text\" name=\"title\" "
            "placeholder=\"Semester 3 Marksheet\" required>";
    form += "<label>File (PDF, JPG or PNG)</label>";
    form += "<input type=\"file\" name=\"file\" "
            "accept=\".pdf,.jpg,.jpeg,.png\" required>";
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
                            const std::string& message,
                            bool documents_tab,
                            bool all_posts)
{
    bool is_owner = student.id == user_id;
    bool show_details = can_see_details(student, user_id);

    std::string html = read_file("templates/profile.html");
    std::string id = std::to_string(student.id);

    html = replace_all(html, "{{NAV}}", nav_html(user_id));
    html = replace_all(html, "{{USERNAME}}", escape_html(student.username));
    html = replace_all(html, "{{PHOTO}}", escape_html(student.photo));

    html = replace_all(html, "{{PRIVATE_BADGE}}",
        student.is_private
            ? "<span class=\"private-badge\">Private</span>"
            : "");

    // Posts, followers and following, as one quiet line.
    std::string stats;
    stats += "<span><b>" + std::to_string(count_posts(student.id))
           + "</b> posts</span>";
    stats += "<span><b>" + std::to_string(count_followers(student.id))
           + "</b> followers</span>";
    stats += "<span><b>" + std::to_string(count_following(student.id))
           + "</b> following</span>";
    html = replace_all(html, "{{STATS}}", stats);

    // Follow, message and edit sit together under the name.
    std::string actions = follow_button_html(student, user_id);

    if (can_message(user_id, student))
    {
        actions += "<a class=\"button-link quiet-link\" href=\"/messages/"
                 + id + "\">Message</a>";
    }

    if (is_owner)
    {
        actions += "<a class=\"button-link quiet-link\" href=\"/edit\">"
                   "Edit profile</a>";
    }

    html = replace_all(html, "{{ACTIONS}}", actions);

    std::string note;

    if (!is_owner && user_id != 0 && !can_message(user_id, student))
    {
        note = "<p class=\"small\">" + escape_html(student.username)
             + " only accepts messages from their followers.</p>";
    }

    html = replace_all(html, "{{NOTE}}", note);

    // Follow requests only appear when somebody is actually waiting.
    std::string requests;

    if (is_owner && count_pending_requests(student.id) > 0)
    {
        requests += "<div class=\"panel\">";
        requests += "<h3>Follow requests</h3>";
        requests += follow_requests_html(student.id);
        requests += "</div>";
    }

    html = replace_all(html, "{{REQUESTS}}", requests);

    if (!show_details)
    {
        // A private profile stops here: a name, a picture and a way to ask.
        html = replace_all(html, "{{CHIPS}}", "");
        html = replace_all(html, "{{BIO}}", "");
        html = replace_all(html, "{{TABS}}", "");
        html = replace_all(html, "{{CONTENT}}",
            "<div class=\"panel locked\">"
            "<h3>This profile is private</h3>"
            "<p class=\"muted\">Follow " + escape_html(student.username)
            + " to see their year, program, marks, bio, posts and documents. "
              "They have to accept your request first.</p></div>");

        return html_page(html);
    }

    html = replace_all(html, "{{CHIPS}}", chip_row_html(student));
    html = replace_all(html, "{{BIO}}",
        student.bio.empty() ? ""
                            : "<p class=\"bio\">" + escape_html(student.bio) + "</p>");

    // Posts and documents share the page through two tabs instead of being
    // stacked one under the other.
    std::string tabs;
    tabs += "<div class=\"tabs\">";
    tabs += "<a class=\"tab";
    tabs += documents_tab ? "" : " active";
    tabs += "\" href=\"/student/" + id + "\">Posts</a>";
    tabs += "<a class=\"tab";
    tabs += documents_tab ? " active" : "";
    tabs += "\" href=\"/student/" + id + "?tab=documents\">Documents</a>";
    tabs += "</div>";

    html = replace_all(html, "{{TABS}}", tabs);

    std::string content;

    if (documents_tab)
    {
        if (is_owner)
        {
            content += add_document_html(message);
        }

        content += documents_html(student.id, is_owner);
    }
    else
    {
        content += profile_posts_html(student.id, user_id,
                                      "/student/" + id, all_posts);
    }

    html = replace_all(html, "{{CONTENT}}", content);

    return html_page(html);
}

// ---------- main ----------

int main()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);

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

    // A database made before sessions had a created_at column needs the
    // column adding. It is already there in a new one, where this fails and
    // the failure is the answer, so it is not checked.
    sqlite3_exec(db,
                 "ALTER TABLE sessions ADD COLUMN created_at TEXT NOT NULL "
                 "DEFAULT '';",
                 nullptr, nullptr, nullptr);

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
            unique_file_name(student.username, "", extension));

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

        const char* tab = req.url_params.get("tab");

        const char* posts = req.url_params.get("posts");

        return profile_page(student, user_id, note ? note : "",
                            tab && std::string(tab) == "documents",
                            posts && std::string(posts) == "all");
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
                unique_file_name(student.username, "", extension));

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
            return redirect_to("/profile?tab=documents&note=Please+add+a+name+and+a+file.");
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

            delete_post_replies(posted_id(req));
            delete_post_notifications(posted_id(req));
            delete_post(posted_id(req), user_id);
        }

        return redirect_to("/profile");
    });

    // A form sends back the page it was pressed on, so a like on the home
    // feed returns to the home feed and one on a profile stays there.
    auto posted_back = [](const crow::request& req)
    {
        crow::query_string form("?" + req.body);
        const char* back = form.get("back");
        return safe_back(back ? back : "/");
    };

    CROW_ROUTE(app, "/post/like").methods(crow::HTTPMethod::POST)
    ([posted_id, posted_back](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        int post_id = posted_id(req);

        toggle_like(post_id, user_id);

        if (viewer_likes(post_id, user_id))
        {
            add_notification(post_id, user_id, "like");
        }
        else
        {
            remove_like_notification(post_id, user_id);
        }

        return redirect_to(posted_back(req));
    });

    CROW_ROUTE(app, "/comment").methods(crow::HTTPMethod::POST)
    ([posted_back](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        crow::query_string form("?" + req.body);
        const char* post_text = form.get("post_id");
        const char* body      = form.get("body");

        if (post_text && body
            && add_comment(std::atoi(post_text), user_id, body))
        {
            add_notification(std::atoi(post_text), user_id, "comment");
        }

        return redirect_to(posted_back(req));
    });

    CROW_ROUTE(app, "/comment/delete").methods(crow::HTTPMethod::POST)
    ([posted_id, posted_back](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        // The SQL only removes a comment you wrote or one under a post of
        // yours, so anybody else pressing this changes nothing.
        delete_comment(posted_id(req), user_id);

        return redirect_to(posted_back(req));
    });

    CROW_ROUTE(app, "/notifications")
    ([](const crow::request& req)
    {
        int user_id = logged_in_id(req);

        if (user_id == 0)
        {
            return redirect_to("/login");
        }

        std::string html = read_file("templates/notifications.html");

        // Drawn first, marked seen afterwards, so the page you are looking
        // at still shows which ones were new.
        html = replace_all(html, "{{NAV}}", nav_html(user_id));
        html = replace_all(html, "{{ALERTS}}", notifications_html(user_id));

        mark_notifications_seen(user_id);

        return html_page(html);
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

        return redirect_to("/profile?tab=documents");
    });

    CROW_ROUTE(app, "/student/<int>")
    ([](const crow::request& req, int id)
    {
        Student student;

        if (!get_student(id, student))
        {
            return crow::response(404, "Student not found.");
        }

        const char* tab = req.url_params.get("tab");

        const char* posts = req.url_params.get("posts");

        return profile_page(student, logged_in_id(req), "",
                            tab && std::string(tab) == "documents",
                            posts && std::string(posts) == "all");
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
