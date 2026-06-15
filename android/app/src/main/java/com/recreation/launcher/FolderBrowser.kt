package com.recreation.launcher

import android.os.Environment
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import java.io.File

/**
 * A filesystem folder browser dialog backed by java.io.File (the app holds
 * all-files access). Starts at [start], lets the user step into subfolders and
 * back up to the storage root, and reports the chosen directory via [onSelect].
 */
@Composable
fun FolderBrowserDialog(
    start: String,
    onSelect: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    val root = remember { Environment.getExternalStorageDirectory() }
    val initial = remember(start) {
        File(start).takeIf { it.isDirectory } ?: root
    }
    var current by remember { mutableStateOf(initial) }
    val subDirs = remember(current) {
        current.listFiles { f -> f.isDirectory && !f.isHidden }
            ?.sortedBy { it.name.lowercase() }
            .orEmpty()
    }
    val canGoUp = current.parentFile != null && current.absolutePath != root.absolutePath

    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Column {
                Text("Choose folder", style = MaterialTheme.typography.titleMedium)
                Text(
                    current.absolutePath,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        },
        text = {
            Column {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    IconButton(onClick = { current = current.parentFile ?: current }, enabled = canGoUp) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Up")
                    }
                    Text(
                        if (canGoUp) "Up one level" else "Storage root",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                HorizontalDivider()
                if (subDirs.isEmpty()) {
                    Box(Modifier.fillMaxWidth().padding(vertical = 24.dp), contentAlignment = Alignment.Center) {
                        Text(
                            "No subfolders here.",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                } else {
                    LazyColumn(Modifier.heightIn(max = 320.dp)) {
                        items(subDirs, key = { it.absolutePath }) { dir ->
                            FolderRow(dir) { current = dir }
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { onSelect(current.absolutePath) }) { Text("Select this folder") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        },
    )
}

@Composable
private fun FolderRow(dir: File, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Icon(
            Icons.Filled.Folder,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.primary,
        )
        Text(
            dir.name,
            style = MaterialTheme.typography.bodyLarge,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}
