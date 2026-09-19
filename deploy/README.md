# Hosting this on a free Oracle Cloud VM

The app is a long-running process with a SQLite file beside it, so it needs a
machine, not static hosting. Oracle's Always Free tier gives one permanently,
which is why it is used here rather than a PaaS free tier - most of those have
no persistent disk, and the database would be wiped on every restart.

## What you do in the Oracle console

1. Create an account at <https://signup.cloud.oracle.com>. A card is asked for
   to verify identity; Always Free resources are not billed to it.
2. **Create an instance**: Compute -> Instances -> Create instance.
   - Image: **Ubuntu 24.04**
   - Shape: **VM.Standard.A1.Flex**, 1-2 OCPU and 6 GB, which is inside the
     free allowance and builds far faster than the 1 GB micro shape. If the
     region reports no ARM capacity, try another region or fall back to
     VM.Standard.E2.1.Micro - the setup script adds swap so it can still build.
   - Save the **SSH private key** it offers. You need it to log in.
3. **Open the ports**: on the instance page follow its subnet, then the default
   security list, and add two ingress rules:
   - Source `0.0.0.0/0`, protocol TCP, destination port **80**
   - Source `0.0.0.0/0`, protocol TCP, destination port **443**

   This is separate from the firewall inside the VM, which the script handles.
   Miss it and the site is unreachable with no error to explain why.

## Then

Give the public IP and the key to Claude, or run it yourself:

```
ssh -i <key> ubuntu@<public-ip>
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Harshrathod176/student-social-clone/main/deploy/setup-server.sh)"
```

It builds the app, installs it as a systemd service so it restarts on reboot,
and puts Caddy in front for HTTPS. First run takes 10-20 minutes, almost all of
it compiling libsodium and Crow.

The site ends up at `https://<public-ip>.sslip.io` - sslip.io resolves any
`<ip>.sslip.io` name to that address, so a real certificate can be issued
without registering a domain. To use your own domain later, point it at the IP
and re-run with `DOMAIN=yourdomain.com sudo -E bash setup-server.sh`.

## Notes

- The database starts empty. People sign up on the live site.
- `data/students.db` and `static/uploads/` are the only state. Back them up by
  copying those two paths off the machine.
- `sudo systemctl status student-profiles` and `journalctl -u student-profiles`
  show what the app is doing.
