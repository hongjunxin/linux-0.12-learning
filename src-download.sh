#!/bin/bash

#URL=$1
URL=http://www.oldlinux.org/Book-Lite/linux-0.12

# Download file and create directory
# $1 url
parse_url()
{
	local url=$1
 	local http_content
	local html
	wget ${url} -P $$
	html=$(ls $$)
	http_content=$(cat $$/${html} | awk '/href="*"/{print $2}' | \
			        awk -F\" '{print $2}' | \
			        awk -F/ '{print $1}')

	rm -rf $$

	local file_name
	local is_regular_flag
	local dir_list

	if [[ -n ${http_content} ]]; then
		for i in ${http_content}; do
			is_regular_flag=$(echo $i | egrep '\.(c|h|s|S)$')
			if [ "X${is_regular_flag}" != "X" -o \
			      $i = "Makefile" -o \
			      $i = "makefile" ]; then
				file_list+=${i}
				file_list+=" "
			else
				dir_list+=${i}
				dir_list+=" "
			fi
		done

		for i in ${file_list}; do
			wget ${url}/${i}
		done

		for i in ${dir_list}; do
			mkdir -p $i
			(cd $i && parse_url ${url}/${i})
		done

	fi
}

if [ "X$1" = "X" ]; then
	echo "Lack url paremeter"
	echo "Use default url"
	#exit -1
fi

parse_url ${URL}
