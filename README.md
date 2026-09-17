# Student Profiles

A college project. Students create a profile with a picture, bio, marks and
PDF certificates. Everyone can browse the home page, search for students and
open their profiles.

## What it uses

- C++ with the Crow library for the web server
- SQLite for the database (two tables: `students` and `certificates`)
- Plain HTML forms and one CSS file (the only JavaScript is the one-line
  "are you sure?" box on the delete button)

## Build

```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/Users/anurag/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

## Run

Run it from the project folder, because it reads `templates/`, `static/` and
`database/schema.sql` using relative paths.

```
./build/student_profiles
```

Then open http://localhost:8090

## Pages

| Address | What it does |
| --- | --- |
| `/` | Home page: recommended students and the newest profiles |
| `/signup` | Form to create a profile (picture, marks, optional certificate) |
| `/login` | Login form |
| `/logout` | Logs out |
| `/profile` | Your own profile, with the edit and upload buttons |
| `/edit` | Form to change your picture, bio, year, program and marks |
| `/certificate/delete` | Deletes one of your own certificates |
| `/search` | Search students by username or program |
| `/student/<id>` | Another student's profile |

## The two tables

`students` holds one row per person. `certificates` holds one row per PDF, and
its `student_id` column says which student it belongs to. That is a one-to-many
relationship: one student can have many certificates.

## How it works

1. The signup form is sent as `multipart/form-data` because it has file inputs.
   The server reads each field with `get_part_by_name`.
2. Uploads are saved into `static/uploads/`, so the browser can open them like
   any other file. Both pictures and certificates include the time they were
   uploaded in the name, like `<username>_1789619099.jpg` and
   `<username>_cert1789619409.pdf`. This does two jobs: a new picture gets a new
   address so the browser does not show the old copy it had saved, and a new
   certificate can never overwrite one that is already there.
3. Only `.jpg`, `.jpeg` and `.png` are accepted for pictures, and only `.pdf`
   for certificates.
4. Login checks the username and password in the database and stores the
   student id in a cookie called `user_id`.
5. Pages are HTML files in `templates/` with placeholders like `{{USERNAME}}`.
   The server reads the file and swaps the placeholders for real values.
6. The home page recommends students on the same program as you. It reads your
   program from the database, then selects other students with that same
   program. If you are not logged in it only shows the newest profiles.
7. The edit and upload buttons only appear on your own profile, because
   `profile.html` is also used for other people's profiles and the server only
   fills in `{{OWNER_SECTION}}` when the profile id matches your cookie.
8. On the edit page the picture box is optional. If you leave it empty the
   server keeps your old picture; if you choose a new one it saves the new file
   and deletes the old one. `/edit` checks your cookie before saving anything,
   so nobody can edit a profile that is not theirs.
9. Deleting a certificate removes the database row and the PDF file. The
   delete query has `WHERE id = ? AND student_id = ?` in it, so even if someone
   sent the id of a certificate that is not theirs, no row would match and
   nothing would be deleted.

## Notes

- Passwords are stored as plain text to keep the code simple. This is fine for a
  class demo but should not be done for a real website.
- The username can only contain letters, numbers and `_`, because it is also
  used in the file names of the uploads.
- The username cannot be changed after signup, because it is the name other
  students know you by and it is part of your upload file names.

## Sharing it with others

- Same WiFi: others can open `http://<your-ip>:8090`
- Anywhere: `cloudflared tunnel --url http://localhost:8090` prints a public
  `https://...trycloudflare.com` link. The link only works while that command
  and the server are both running.
