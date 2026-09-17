CREATE TABLE IF NOT EXISTS students (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL UNIQUE,
    password TEXT NOT NULL,
    bio TEXT NOT NULL,
    photo TEXT NOT NULL,
    year TEXT NOT NULL,
    program TEXT NOT NULL,
    marks TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS certificates (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    student_id INTEGER NOT NULL,
    title TEXT NOT NULL,
    file_path TEXT NOT NULL,
    FOREIGN KEY (student_id) REFERENCES students(id)
);
