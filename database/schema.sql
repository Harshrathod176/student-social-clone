CREATE TABLE IF NOT EXISTS students (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL UNIQUE,
    password TEXT NOT NULL,
    bio TEXT NOT NULL,
    photo TEXT NOT NULL,
    year TEXT NOT NULL,
    program TEXT NOT NULL,
    marks TEXT NOT NULL,
    is_private INTEGER NOT NULL DEFAULT 0,
    dm_open INTEGER NOT NULL DEFAULT 1
);

-- Any student document: a marksheet, a certificate, a transcript, anything.
CREATE TABLE IF NOT EXISTS documents (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    student_id INTEGER NOT NULL,
    title TEXT NOT NULL,
    file_path TEXT NOT NULL,
    FOREIGN KEY (student_id) REFERENCES students(id)
);

-- Short updates a student writes, with an optional file attached.
CREATE TABLE IF NOT EXISTS posts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    student_id INTEGER NOT NULL,
    body TEXT NOT NULL,
    file_path TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (student_id) REFERENCES students(id)
);

-- One row per direct message.
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sender_id INTEGER NOT NULL,
    receiver_id INTEGER NOT NULL,
    body TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (sender_id) REFERENCES students(id),
    FOREIGN KEY (receiver_id) REFERENCES students(id)
);

CREATE TABLE IF NOT EXISTS follows (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    follower_id INTEGER NOT NULL,
    following_id INTEGER NOT NULL,
    status TEXT NOT NULL,
    UNIQUE (follower_id, following_id),
    FOREIGN KEY (follower_id) REFERENCES students(id),
    FOREIGN KEY (following_id) REFERENCES students(id)
);
