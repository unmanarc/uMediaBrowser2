"""Write temporary Boost INFO settings for integration server processes."""
from pathlib import Path


def config_command(binary, directory, **settings):
    def quote(value):
        if isinstance(value, bool):
            value = 'true' if value else 'false'
        return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'
    path = Path(directory) / 'server.info'
    groups = {}
    for key, value in settings.items():
        group, leaf = key.split('.', 1)
        groups.setdefault(group, []).append(f'    {leaf} {quote(value)}\n')
    path.write_text(''.join(group + '\n{\n' + ''.join(lines) + '}\n'
                            for group, lines in groups.items()))
    return [str(binary), str(path)]
