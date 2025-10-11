import subprocess
import time
import json
import os
import signal
import threading


def stream_reader(stream, stream_name):
    """Reads from a stream and prints to the console."""
    for line in iter(stream.readline, ''):
        print(f"[{stream_name}] {line.strip()}", flush=True)
    stream.close()


def main():
    """
    Starts the scrcpy process and sends touch events to its stdin.
    """
    # The project root is the parent directory of the 'tests' directory
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # Command to run scrcpy. The 'run' script is in the project root.
    command = ["./x/app/scrcpy"]

    print(f"Working directory: {project_root}")
    print(f"Starting process: {' '.join(command)}")

    # Set up environment variables for scrcpy
    env = os.environ.copy()
    # The BUILDDIR is 'x'
    env["PYTHONUNBUFFERED"] = "1"
    env["SCRCPY_ICON_PATH"] = "app/data/icon.png"
    env["SCRCPY_SERVER_PATH"] = "x/server/scrcpy-server"

    # Start the scrcpy process
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,  # Work with text streams
        bufsize=1,  # Line-buffered
        cwd=project_root,  # Run from the project root directory
        preexec_fn=os.setsid,  # To be able to kill the process group
        env=env
    )

    # Threads to read stdout and stderr in real-time
    stderr_thread = threading.Thread(target=stream_reader, args=(process.stderr, "STDERR"))
    stdout_thread = threading.Thread(target=stream_reader, args=(process.stdout, "STDOUT"))
    stdout_thread.start()
    stderr_thread.start()

    print(f"Process started with PID: {process.pid}. Waiting 5 seconds for it to initialize...")
    time.sleep(5)

    try:

        # Loop to send events
        print(f"\n--- Sequence ---")

        # Initial coordinates
        x, y = 500, 1200

        # 1. ActionDown
        down_event = {"event": "ActionDown", "data": {"x": x, "y": y}}
        down_line = f"LAEvent:{json.dumps(down_event)}\n"
        print(f"Sending: {down_line.strip()}")
        process.stdin.write(down_line)
        process.stdin.flush()
        time.sleep(0.01)

        for j in range(10):
            # 2. ActionMove
            x -= 50
            y += 0
            move_event = {"event": "ActionMove", "data": {"x": x, "y": y}}
            move_line = f"LAEvent:{json.dumps(move_event)}\n"
            print(f"Sending: {move_line.strip()}")
            process.stdin.write(move_line)
            process.stdin.flush()
            time.sleep(0.01)

        # 3. ActionUp
        up_event = {"event": "ActionUp", "data": {"x": x, "y": y}}
        up_line = f"LAEvent:{json.dumps(up_event)}\n"
        print(f"Sending: {up_line.strip()}")
        process.stdin.write(up_line)
        process.stdin.flush()

        time.sleep(5)

    except (BrokenPipeError, KeyboardInterrupt):
        print("\nProcess stdin closed or interrupted.")
    finally:
        print("Terminating scrcpy process...")
        # Kill the whole process group to ensure scrcpy and its children are terminated
        os.killpg(os.getpgid(process.pid), signal.SIGTERM)
        process.wait(timeout=5)

        # Wait for reader threads to finish
        stdout_thread.join()
        stderr_thread.join()

        print("Process terminated.")


if __name__ == "__main__":
    main()
