# Replaces Buildroot's bare "# " prompt: root@my-distro:~# in teal and light blue.
# The language: the session's (Settings, Language), else English
export LANG="${LANG:-en_US.UTF-8}"
PS1='\[\e[1;36m\]\u@\h\[\e[0m\]:\[\e[1;94m\]\w\[\e[0m\]\$ '
