import sys
import tempfile
import threading
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import vpg


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="vpg_python_smoke_") as data_dir:
        with vpg.get_server(data_dir) as pg:
            pg.query("CREATE TABLE py_people (id INT PRIMARY KEY, name TEXT NOT NULL);")
            pg.query("INSERT INTO py_people (id, name) VALUES (1, 'Ada'), (2, 'Grace');")
            result = pg.query("SELECT id, name FROM py_people ORDER BY id;")
            print(result, end="")
            assert "1,Ada" in result
            assert "2,Grace" in result
            pg.vacuum()
            pg.analyze()
            pg.maintain()

            def insert_from_connection(worker_id: int) -> None:
                with vpg.EmbeddedPostgres(data_dir) as worker_pg:
                    worker_pg.query(
                        f"INSERT INTO py_people (id, name) VALUES ({worker_id + 10}, 'worker-{worker_id}');"
                    )

            threads = [
                threading.Thread(target=insert_from_connection, args=(worker_id,))
                for worker_id in range(4)
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()

            count = pg.query("SELECT count(*) FROM py_people;")
            assert "6" in count


if __name__ == "__main__":
    main()
