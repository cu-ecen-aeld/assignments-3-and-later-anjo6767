#!/bin/sh
# finder.sh
# Usage: ./finder.sh <filesdir> <searchstr>

# Check args
# Expect exactly 2 arguments. If not, print error to stderr and exit 1.
if [ $# -ne 2 ]; then
    echo "Error: two arguments required: <filesdir> <searchstr>" >&2
    exit 1
fi

filesdir="$1"   #1st arg is the directory to search
searchstr="$2"	#2nd arg is the fixed string to match

#for debugging
#echo $1
#echo $2


#Ensure the first argument is an existing directory
if [ ! -d "$filesdir" ]; then
    echo "Error: '$filesdir' is not a directory." >&2
    exit 1
fi


# Count regular files under filesdir (recursively)
# Pipeline explanation:
#   find "$filesdir" -type f -print: prints one path per file
#   wc -l: counts lines = number of files
#   | tr -d '[:space:]': strips spaces/newline to get a clean integer
file_count=$(find "$filesdir" -type f -print | wc -l | tr -d '[:space:]')


echo $file_count

# Count matching lines containing searchstr across those files
# -R: recursive
# -F: fixed-string match (not regex)
# --: end of options (handles strings starting with '-')
# "$filesdir" :starting directory to search
#  grep ... | wc -l -> total number of matching lines across all files
#  tr -d '[:space:]': strips spaces/newline to get a clean integer
match_count=$(grep -R -F -- "$searchstr" "$filesdir" | wc -l | tr -d '[:space:]')

echo "The number of files are $file_count and the number of matching lines are $match_count"

