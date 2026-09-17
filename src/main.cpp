#include <crow.h>
#include <sqlite3.h>

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

// Returns 0 when nobody is logged in.
int logged_in_id(const crow::request& req)
{
    std::string value = get_cookie(req, "user_id");

    if (value.empty())
    {
        return 0;
    }

    for (char c : value)
    {
        if (c < '0' || c > '9')
        {
            return 0;
        }
    }

    return std::stoi(value);
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
};

std::string column_text(sqlite3_stmt* stmt, int index)
{
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : "";
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
        "UPDATE students SET bio = ?, year = ?, program = ?, marks = ?, photo = ? "
        "WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    sqlite3_bind_text(stmt, 1, student.bio.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, student.year.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, student.program.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, student.marks.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, student.photo.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, student.id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool get_student(int id, Student& student)
{
    const char* sql =
        "SELECT id, username, bio, photo, year, program, marks "
        "FROM students WHERE id = ?;";

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
        student.marks    = column_text(stmt, 6);
        found = true;
    }

    sqlite3_finalize(stmt);
    return found;
}

// Checks the login details and gives back the student id, or 0.
int check_login(const std::string& username, const std::string& password)
{
    const char* sql = "SELECT id FROM students WHERE username = ? AND password = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);

    int id = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        id = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return id;
}

void add_certificate(int student_id,
                     const std::string& title,
                     const std::string& file_path)
{
    const char* sql =
        "INSERT INTO certificates(student_id, title, file_path) VALUES(?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    sqlite3_bind_int(stmt, 1, student_id);
    sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, file_path.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// Saves the PDF and the database row together. Used by signup and by the
// "add certificate" form on your own profile.
bool save_certificate(int student_id,
                      const std::string& username,
                      const std::string& title,
                      const crow::multipart::part& file)
{
    if (title.empty() || file.body.empty() || !is_pdf(uploaded_file_name(file)))
    {
        return false;
    }

    // The time is part of the name so a new certificate never overwrites
    // one that is already saved.
    std::string saved_name =
        username + "_cert" + std::to_string(std::time(nullptr)) + ".pdf";

    add_certificate(student_id, title, save_upload(file, saved_name));
    return true;
}

// Finds a certificate, but only if it belongs to this student. This is what
// stops someone deleting another student's certificate.
bool find_own_certificate(int certificate_id,
                          int student_id,
                          std::string& file_path)
{
    const char* sql =
        "SELECT file_path FROM certificates WHERE id = ? AND student_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, certificate_id);
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

void delete_certificate(int certificate_id, int student_id)
{
    const char* sql = "DELETE FROM certificates WHERE id = ? AND student_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, certificate_id);
    sqlite3_bind_int(stmt, 2, student_id);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
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
        links += "<a href=\"/profile\">My Profile</a>";
        links += "<a class=\"nav-button\" href=\"/logout\">Logout</a>";
    }

    return links;
}

// Turns the rows of a query into a grid of student cards.
// The query must select: id, username, photo, year, program.
std::string render_cards(sqlite3_stmt* stmt)
{
    std::string cards;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        std::string id       = std::to_string(sqlite3_column_int(stmt, 0));
        std::string username = escape_html(column_text(stmt, 1));
        std::string photo    = escape_html(column_text(stmt, 2));
        std::string year     = escape_html(column_text(stmt, 3));
        std::string program  = escape_html(column_text(stmt, 4));

        cards += "<a class=\"card\" href=\"/student/" + id + "\">";
        cards += "<img class=\"card-photo\" src=\"" + photo + "\" alt=\"\">";
        cards += "<h3>" + username + "</h3>";
        cards += "<p class=\"muted\">" + program + "</p>";

        if (!year.empty())
        {
            cards += "<span class=\"chip\">" + year + "</span>";
        }

        cards += "</a>";
    }

    return cards;
}

std::string newest_students_html()
{
    const char* sql =
        "SELECT id, username, photo, year, program FROM students "
        "ORDER BY id DESC LIMIT 8;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    std::string cards = render_cards(stmt);

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
        "SELECT id, username, photo, year, program FROM students "
        "WHERE program = ? AND id != ? LIMIT 8;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, me.program.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, user_id);

    std::string cards = render_cards(stmt);

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

std::string certificates_html(int student_id, bool is_owner)
{
    const char* sql =
        "SELECT id, title, file_path FROM certificates WHERE student_id = ? "
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

        list += "<div class=\"certificate\">";
        list += "<a class=\"certificate-link\" href=\"" + path
              + "\" target=\"_blank\">";
        list += "<span class=\"pdf-tag\">PDF</span>";
        list += "<span>" + title + "</span>";
        list += "</a>";

        // The delete button is only drawn on your own profile.
        if (is_owner)
        {
            list += "<form action=\"/certificate/delete\" method=\"post\">";
            list += "<input type=\"hidden\" name=\"id\" value=\"" + id + "\">";
            list += "<button class=\"delete-button\" type=\"submit\" "
                    "onclick=\"return confirm('Delete this certificate?')\">"
                    "Delete</button>";
            list += "</form>";
        }

        list += "</div>";
    }

    sqlite3_finalize(stmt);

    if (list.empty())
    {
        return "<p class=\"empty\">No certificates added yet.</p>";
    }

    return list;
}

// The upload form only appears when you are looking at your own profile.
std::string owner_section_html(bool is_owner, const std::string& message)
{
    if (!is_owner)
    {
        return "";
    }

    std::string form;

    form += "<div class=\"panel\">";
    form += "<h3>Your profile</h3>";
    form += "<p class=\"muted\">Only you can see these buttons.</p>";
    form += "<a class=\"button-link\" href=\"/edit\">Edit profile</a>";
    form += "</div>";

    form += "<div class=\"panel\">";
    form += "<h3>Add a certificate</h3>";

    if (!message.empty())
    {
        form += "<p class=\"message\">" + escape_html(message) + "</p>";
    }

    form += "<form action=\"/certificate\" method=\"post\" "
            "enctype=\"multipart/form-data\">";
    form += "<label>Certificate name</label>";
    form += "<input type=\"text\" name=\"title\" placeholder=\"Python Basics\" required>";
    form += "<label>PDF file</label>";
    form += "<input type=\"file\" name=\"file\" accept=\".pdf\" required>";
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

    return html_page(html);
}

crow::response profile_page(const Student& student,
                            int user_id,
                            const std::string& message)
{
    bool is_owner = student.id == user_id;

    std::string html = read_file("templates/profile.html");

    html = replace_all(html, "{{NAV}}", nav_html(user_id));
    html = replace_all(html, "{{USERNAME}}", escape_html(student.username));
    html = replace_all(html, "{{PHOTO}}", escape_html(student.photo));
    html = replace_all(html, "{{BIO}}", escape_html(student.bio));
    html = replace_all(html, "{{CHIPS}}", chip_row_html(student));
    html = replace_all(html, "{{CERTIFICATES}}",
                       certificates_html(student.id, is_owner));
    html = replace_all(html, "{{OWNER_SECTION}}", owner_section_html(is_owner, message));

    return html_page(html);
}

// ---------- main ----------

int main()
{
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

        std::string html = read_file("templates/home.html");

        html = replace_all(html, "{{NAV}}", nav_html(user_id));
        html = replace_all(html, "{{RECOMMENDED}}", recommended_html(user_id));
        html = replace_all(html, "{{NEWEST}}", newest_students_html());

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

        if (password.empty())
        {
            return form_page("templates/signup.html", 0, "Please enter a password.");
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

        student.photo = save_upload(picture,
                                    photo_file_name(student.username, extension));

        if (!add_student(student, password))
        {
            return form_page("templates/signup.html", 0,
                             "Could not save your profile. Please try again.");
        }

        // A certificate on the signup form is optional.
        int new_id = static_cast<int>(sqlite3_last_insert_rowid(db));

        save_certificate(new_id,
                         student.username,
                         form.get_part_by_name("cert_title").body,
                         form.get_part_by_name("cert_file"));

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

        crow::response res = redirect_to("/profile");
        res.set_header("Set-Cookie", "user_id=" + std::to_string(id) + "; Path=/");
        return res;
    });

    CROW_ROUTE(app, "/logout")
    ([]
    {
        crow::response res = redirect_to("/login");
        res.set_header("Set-Cookie", "user_id=; Path=/; Max-Age=0");
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

        // Choosing a new picture is optional. An empty box keeps the old one.
        crow::multipart::part picture = form.get_part_by_name("photo");

        if (!picture.body.empty())
        {
            std::string extension = picture_extension(uploaded_file_name(picture));

            if (extension.empty())
            {
                return edit_page(student, "A picture must be a .jpg or .png file.");
            }

            student.photo = save_upload(picture,
                                        photo_file_name(student.username, extension));

            // The old picture is not needed any more.
            std::remove(("." + old_photo).c_str());
        }

        update_student(student);

        return redirect_to("/profile");
    });

    CROW_ROUTE(app, "/certificate").methods(crow::HTTPMethod::POST)
    ([](const crow::request& req)
    {
        int user_id = logged_in_id(req);
        Student student;

        if (user_id == 0 || !get_student(user_id, student))
        {
            return redirect_to("/login");
        }

        crow::multipart::message form(req);

        bool saved = save_certificate(user_id,
                                      student.username,
                                      form.get_part_by_name("title").body,
                                      form.get_part_by_name("file"));

        if (!saved)
        {
            return redirect_to("/profile?note=Please+add+a+name+and+a+PDF+file.");
        }

        return redirect_to("/profile");
    });

    CROW_ROUTE(app, "/certificate/delete").methods(crow::HTTPMethod::POST)
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

        int certificate_id = std::atoi(id_text);
        std::string file_path;

        // Nothing happens unless the certificate is really yours.
        if (find_own_certificate(certificate_id, user_id, file_path))
        {
            std::remove(("." + file_path).c_str());
            delete_certificate(certificate_id, user_id);
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
            "SELECT id, username, photo, year, program FROM students "
            "WHERE username LIKE ? OR program LIKE ? ORDER BY username;";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

        std::string pattern = "%" + query + "%";
        sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);

        std::string cards = render_cards(stmt);
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
