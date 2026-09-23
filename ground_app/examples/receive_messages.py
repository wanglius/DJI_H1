"""安装后运行：python examples/receive_messages.py config/ground_app.local.json"""
import json
import sys
from dji_h1_ground import GroundReceiver


def main():
    receiver = GroundReceiver.from_config(sys.argv[1])
    try:
        with receiver:
            for message in receiver.iter_messages():
                print(json.dumps(message.to_dict(), ensure_ascii=False), flush=True)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
