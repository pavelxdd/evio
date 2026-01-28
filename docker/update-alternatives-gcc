#!/usr/bin/env bash
set -e

version="$1"
priority="$2"

pkgs=(
  "gcc-${version}"
  "g++-${version}"
  "cpp-${version}"
  "c++-${version}"
)

for pkg in "${pkgs[@]}"; do
  if [ "$(dpkg-query -W -f='${Status}' "${pkg}" 2> /dev/null | grep -c "ok installed")" -eq 1 ]; then
    dpkg-query -L "${pkg}" | grep "^/usr/bin/" | grep "\-${version}\$" | while read -r link; do
      path="${link%-"${version}"}"
      name="$(basename "${path}")"
      update-alternatives --force --remove-all "${name}" 2> /dev/null | true
      update-alternatives --force --install "${path}" "${name}" "${link}" "${priority}"
      update-alternatives --auto "${name}"
    done
  fi
done

update-alternatives --force --remove-all cc 2> /dev/null | true
update-alternatives --force --install /usr/bin/cc cc /usr/bin/gcc "${priority}"
update-alternatives --force --set cc /usr/bin/gcc

update-alternatives --force --remove-all cxx 2> /dev/null | true
update-alternatives --force --install /usr/bin/cxx cxx /usr/bin/g++ "${priority}"
update-alternatives --force --set cxx /usr/bin/g++

update-alternatives --force --remove-all c++ 2> /dev/null | true
update-alternatives --force --install /usr/bin/c++ c++ /usr/bin/g++ "${priority}"
update-alternatives --force --set c++ /usr/bin/g++
