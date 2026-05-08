import sys
import tempfile
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


if __name__ == "__main__":
    main()
