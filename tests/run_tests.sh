#!/usr/bin/env bash
# Runs each test's input through the shell and compares the output with what we expect.

cd "$(dirname "$0")/.." || exit 1
SHELL_BIN="$(pwd)/shell"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

# check <name> <input> <expected> [sed filter applied to the output]
check() {
    local name=$1 input=$2 expected=$3 filter=${4:-}
    local actual
    actual=$(cd "$TMP" && printf '%s\n' "$input" | "$SHELL_BIN" 2>&1)
    [ -n "$filter" ] && actual=$(printf '%s\n' "$actual" | sed -E "$filter")

    if [ "$actual" == "$expected" ]; then
        pass=$((pass + 1))
        echo "  ok    $name"
    else
        fail=$((fail + 1))
        echo "  FAIL  $name"
        echo "        expected: $(printf '%q' "$expected")"
        echo "        actual:   $(printf '%q' "$actual")"
    fi
}

echo "basics"
check "echo"                     'echo hello'                    'hello'
check "arguments"                'printf "%s-%s\n" a b'          'a-b'
check "command not found"        'nosuchcmd'                     'nosuchcmd: command not found'
check "cd and pwd"               $'cd /\npwd'                    '/'
check "cd with no args"          $'cd\npwd'                      "$HOME"

echo "redirection"
check "> and >> and <"           $'echo first > f.txt\necho second >> f.txt\ncat < f.txt'  $'first\nsecond'
check "2> file"                  $'ls /nonexistent 2> err.txt\nwc -l < err.txt'            '1'
check "2>&1 into a pipe"         'ls /nonexistent 2>&1 | wc -l'                            '1'
check "no spaces around ops"     'echo hi>g.txt;cat<g.txt'                                 'hi'
check "missing file name"        'echo hi >'                     'syntax error: expected file name after redirect'

echo "pipes"
check "two commands"             'printf "c\nb\na\nb\n" | sort | uniq'   $'a\nb\nc'
check "three commands"           'seq 1 10 | grep 1 | wc -l'             '2'
check "early exit doesn't hang"  'seq 1 1000000 | head -1'              '1'
check "built-in in a pipeline"   $'cd /\npwd | cat'                      '/'
check "empty pipe stage"         'ls | | wc'                             "syntax error near '|'"

echo "quoting and variables"
check "double quotes"            'echo "a   b"'                   'a   b'
check "single quotes"            "echo '\$HOME'"                 '$HOME'
check "quoted pipe is text"      'echo "a | b"'                  'a | b'
check "backslash"                'echo a\ b'                     'a b'
check "export and expand"        'export NAME=world; echo "hello $NAME"'   'hello world'
check "unset"                    'export X=1; unset X; echo "[$X]"'        '[]'
check "unset var vs empty quotes" 'printf "[%s]" $NOPE ""'       '[]'
check "tilde"                    'echo ~'                        "$HOME"
check "unterminated quote"       'echo "abc'                     'syntax error: unterminated quote'

echo "chaining and exit status"
check "exit status \$?"          $'false; echo $?\ntrue; echo $?\nnosuchcmd 2>/dev/null; echo $?'  $'1\n0\n127'
check "&& and ||"                $'true && echo a\nfalse && echo b\nfalse || echo c\nfalse && echo d || echo e'  $'a\nc\ne'
check "semicolon"                'echo 1; echo 2;'               $'1\n2'
check "empty command"            'echo a ;; echo b'              'syntax error: missing command'

echo "history"
check "!!"                       $'echo hi\n!!'                  $'hi\necho hi\nhi'
check "!n"                       $'echo one\necho two\n!1'       $'one\ntwo\necho one\none'
check "history list"             $'echo x\nhistory'              $'x\n    1  echo x\n    2  history'

echo "background jobs"
check "& and jobs"               $'sleep 0.3 &\njobs'            $'[1] PID\n[1] Running    sleep 0.3 &'   's/^\[1\] [0-9]+$/[1] PID/'
check "done message"             $'sleep 0.1 &\nsleep 0.3\necho after'  $'[1] PID\n[1] Done    sleep 0.1\nafter'  's/^\[1\] [0-9]+$/[1] PID/'

echo "exit"
printf 'exit 3\n' | "$SHELL_BIN" > /dev/null
if [ $? -eq 3 ]; then
    pass=$((pass + 1)); echo "  ok    exit code"
else
    fail=$((fail + 1)); echo "  FAIL  exit code"
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
