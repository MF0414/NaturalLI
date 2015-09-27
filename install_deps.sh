#!/usr/bin/env bash
#

set -o nounset
set -o errexit

# Helper to make sure programs exist
ensureCmd() {
  command -v $1 >/dev/null 2>&1 || {
    err >&2 "The program '$1' is not in your path (or not executable)! $2";
    return 1
  }
}

# Some colors, because colors are awesome!
color_clear='\033[0m'
color_red='\033[31;1m'
function red() {
  echo -e "${color_red}$@${color_clear}"
}
color_green='\033[32;1m'
function green() {
  echo -e "${color_green}$@${color_clear}" 
}
color_yellow='\033[33;1m'
function yellow() {
  echo -e "${color_yellow}$@${color_clear}" 
}

# Determine OS platform
UNAME=$(uname | tr "[:upper:]" "[:lower:]")
# If Linux, try to determine specific distribution
if [ "$UNAME" == "linux" ]; then
    # If available, use LSB to identify distribution
    if [ -f /etc/lsb-release -o -d /etc/lsb-release.d ]; then
        export DISTRO=$(lsb_release -i | cut -d: -f2 | sed s/'^\t'//)
    # Otherwise, use release info file
    else
        export DISTRO=$(ls -d /etc/[A-Za-z]*[_-][rv]e[lr]* | grep -v "lsb" | cut -d'/' -f3 | cut -d'-' -f1 | cut -d'_' -f1)
    fi
fi
# For everything else (or if above failed), just use generic identifier
[ "$DISTRO" == "" ] && export DISTRO=$UNAME
unset UNAME

echo "$DISTRO"

if [ "$DISTRO" == "Ubuntu" ]; then
  echo "Installing dependencies..."
  yellow "(this will need sudo access...)"
  set -o xtrace
  sleep 3
  sudo apt-get install pv libtool build-essential autoconf doxygen ant
  set +o xtrace
else
  yellow ""
  yellow "COULD NOT AUTOMATICALLY INSTALL DEPENDENCIES!"
  yellow ""
fi


# Check to make sure requisite programs are there
echo "Checking programs..."
ensureCmd libtoolize
ensureCmd aclocal
ensureCmd autoheader
ensureCmd autoconf
ensureCmd automake
ensureCmd pv
ensureCmd make
ensureCmd doxygen
ensureCmd ant

green "Everyting is OK!"
