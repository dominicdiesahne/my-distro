# Recovery mode (the boot menu's "recovery mode" entry): says what can be
# done here, after logging in on the text console.
if grep -qw my-recovery /proc/cmdline 2>/dev/null; then
	cat <<'EOF'

  === my-distro recovery mode: text console, no desktop ===

  my-update rollback   undo the last system update
  my-reset             factory reset: delete all users, files and settings
  passwd NAME          set a new password for NAME (as root)
  my-logs is not here: the logs are in /var/log (less /var/log/messages)
  reboot               start normally again;  poweroff   switch off

EOF
fi
