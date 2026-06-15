package com.recreation.launcher

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.recreation.launcher.ui.theme.RecreationTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Direct-launch path (e.g. adb-driven verification): skip the UI and go
        // straight into the renderer, showing over the keyguard if locked.
        if (intent?.getBooleanExtra("launch", false) == true) {
            setShowWhenLocked(true)
            startActivity(Intent(this, GameActivity::class.java))
            finish()
            return
        }
        enableEdgeToEdge()
        setContent {
            RecreationTheme {
                LauncherScreen()
            }
        }
    }
}

private fun hasAllFilesAccess(): Boolean =
    Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LauncherScreen() {
    val context = LocalContext.current
    var config by remember { mutableStateOf(GameConfig.load(context)) }
    var storageGranted by remember { mutableStateOf(hasAllFilesAccess()) }

    Scaffold(
        topBar = {
            TopAppBar(title = {
                Text("recreation", style = MaterialTheme.typography.titleLarge)
            })
        },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                text = { Text("Launch") },
                icon = { Icon(Icons.Filled.PlayArrow, contentDescription = null) },
                onClick = {
                    config.save(context)
                    config.writeNativeConfig(context)
                    context.startActivity(Intent(context, GameActivity::class.java))
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 16.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Spacer(Modifier.height(4.dp))

            StorageCard(granted = storageGranted, onGrant = {
                storageGranted = requestAllFilesAccess(context)
            })

            LaunchTargetCard(config = config, onChange = { config = it })

            GamePathsCard(config = config, onChange = { config = it })

            AdvancedCard(config = config, onChange = { config = it })

            Spacer(Modifier.height(80.dp))
        }
    }
}

@Composable
private fun SectionCard(title: String, content: @Composable () -> Unit) {
    Card(elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            content()
        }
    }
}

@Composable
private fun StorageCard(granted: Boolean, onGrant: () -> Unit) {
    SectionCard("Storage access") {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            val icon: ImageVector = if (granted) Icons.Filled.CheckCircle else Icons.Filled.Warning
            Icon(icon, contentDescription = null)
            Text(
                if (granted) "All-files access granted." else "Needed to read game data off shared storage.",
                modifier = Modifier.weight(1f),
                style = MaterialTheme.typography.bodyMedium,
            )
            if (!granted) Button(onClick = onGrant) { Text("Grant") }
        }
    }
}

@Composable
private fun LaunchTargetCard(config: GameConfig, onChange: (GameConfig) -> Unit) {
    SectionCard("Launch") {
        for (game in GameId.entries) {
            val selected = (config.target as? LaunchTarget.Game)?.id == game
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .selectable(selected = selected) {
                        onChange(config.copy(target = LaunchTarget.Game(game)))
                    },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                RadioButton(selected = selected, onClick = {
                    onChange(config.copy(target = LaunchTarget.Game(game)))
                })
                Text(game.display, style = MaterialTheme.typography.bodyLarge)
            }
        }

        val demoSelected = config.target is LaunchTarget.Demo
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .selectable(selected = demoSelected) {
                    onChange(config.copy(target = LaunchTarget.Demo("materials")))
                },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            RadioButton(selected = demoSelected, onClick = {
                onChange(config.copy(target = LaunchTarget.Demo("materials")))
            })
            Text("Demo scene (no assets needed)", style = MaterialTheme.typography.bodyLarge)
        }

        if (demoSelected) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                val current = (config.target as LaunchTarget.Demo).scene
                DEMO_SCENES.forEach { scene ->
                    FilterChip(
                        selected = scene == current,
                        onClick = { onChange(config.copy(target = LaunchTarget.Demo(scene))) },
                        label = { Text(scene) },
                    )
                }
            }
        }
    }
}

@Composable
private fun GamePathsCard(config: GameConfig, onChange: (GameConfig) -> Unit) {
    SectionCard("Game data paths") {
        Text(
            "Point each game at its Data folder on the device (e.g. " +
                "/storage/emulated/0/recreation/SkyrimSE/Data).",
            style = MaterialTheme.typography.bodySmall,
        )
        for (game in GameId.entries) {
            OutlinedTextField(
                value = config.dataDir(game),
                onValueChange = {
                    onChange(config.copy(dataDirs = config.dataDirs + (game to it)))
                },
                label = { Text(game.display) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
        }
    }
}

@Composable
private fun AdvancedCard(config: GameConfig, onChange: (GameConfig) -> Unit) {
    SectionCard("Advanced") {
        ToggleRow(
            label = "Vulkan validation layers",
            description = "Verify the renderer emits no validation errors.",
            checked = config.validation,
        ) { onChange(config.copy(validation = it)) }
        ToggleRow(
            label = "Ray tracing",
            description = "Off by default; mobile GPUs lack the hardware path.",
            checked = config.raytracing,
        ) { onChange(config.copy(raytracing = it)) }
    }
}

@Composable
private fun ToggleRow(label: String, description: String, checked: Boolean, onCheck: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.bodyLarge)
            Text(
                description,
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Default,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Switch(checked = checked, onCheckedChange = onCheck)
    }
}

private fun requestAllFilesAccess(context: android.content.Context): Boolean {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
        val intent = Intent(
            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            Uri.parse("package:${context.packageName}"),
        )
        context.startActivity(intent)
        return false
    }
    return true
}
