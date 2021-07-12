# osu-cs344-smallsh

[![A screenshot of this program.](http://georgethomashill.com/gh/osu/cs344/cs344-smallsh-screenshot.png "Click to see screencast.")](http://georgethomashill.com/gh/osu/cs344/cs344-smallsh-screencast.mp4)

CS 344 was Oregon State University's Operating Systems I course.

For this project, I wrote "smallsh", a simple shell implemented in C that had to meet numerous requirements.

It had three built-in commands—exit, cd, and status—and gave access to other GNU/Linux commands by allowing the user to spawn and terminate foreground and background subprocesses.

It also allowed the user to navigate a server's file structure, and it responded to signals such as SIGINT and SIGTSTP.

It used the colon as its command prompt.

Compile this program with:

```
gcc -o smallsh smallsh.c
```

Run it with:

```
smallsh
```

A screencast of the program's operations can be viewed [here](http://georgethomashill.com/gh/osu/cs344/cs344-smallsh-screencast.mp4).
