# Programs with limited permissions (Settings, Permissions) start through
# their starter in ~/.local/share/my-run/bin, also from a terminal (1.3.0,
# see my-run). /etc/profile sets PATH again for login shells.
if [ "$(id -u)" != 0 ] && [ -n "$HOME" ]; then
	case ":$PATH:" in
	*":$HOME/.local/share/my-run/bin:"*) ;;
	*) PATH="$HOME/.local/share/my-run/bin:$PATH" ;;
	esac
	export PATH
fi
