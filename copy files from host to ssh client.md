## Correct syntax

```
scp .\jetson_router.c user_4@10.4.136.90:~/
```

The colon right after the IP address is what tells `scp` everything after it is a path on the remote machine, `~/` here means drop it directly into your home directory on the Ubuntu box.

Run this, enter the password when prompted, and it should show a transfer progress line confirming it copied.

## Where to find it after a successful transfer

SSH into the Ubuntu box if you're not already connected:
```
ssh user_4@10.4.136.90
```
Then:
```bash
ls -la ~/jetson_router.c
```
That confirms it landed in your home directory. From there, move it into the same folder as your other DPDK code files to keep things organized:
```bash
mv ~/jetson_router.c ~/dpdk/code-files/
cd ~/dpdk/code-files
ls
```
You should see it sitting alongside `access_internet.c` and the others.

Try the corrected `scp` command with the colon, then run the `ls` check and tell me what you see.
