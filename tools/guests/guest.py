"""GuestData: what a disc's pipeline hands the builder for one guest character."""
from dataclasses import dataclass, field
from typing import Callable


@dataclass
class GuestData:
    id: int                          # the character ID on GC: 29 Spawn, 28 Heihachi
    cell: int                        # his select cell
    name: str                        # for messages
    block: dict                      # slot in his 39-record block -> GC entry
    scenes: dict                     # group 4 record -> GC entry (his home-stage scene)
    ending_art: bytes                # group 2 record 57 + id
    faces: list                      # items as ui.append_textures takes them, one per costume
    silhouettes: list                # the same
    camera: Callable[[bytes], bytes]  # GC profile-camera entry -> the entry with his slots filled
    tables: dict                     # his manifest rows; the builder fills the cell's faces and silhouettes
    pool_lists: list = field(default_factory=list)  # the other disc's ID lists, which place him beside Link
    sources: dict = field(default_factory=dict)     # release and source hashes, for "already built"
