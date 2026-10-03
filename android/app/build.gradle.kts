plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "com.zqr.localnetviewer"
    // androidx.core 1.19.0 / lifecycle 2.11.0 要求 compileSdk ≥ 37（AAR metadata 校验），
    // 否则 :app:checkDebugAarMetadata 直接失败。targetSdk 保持 36 不变。
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.zqr.localnetviewer"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }

    lint {
        // 只把这些**有意为之**的告警降级，其余保持默认（当前 0 error）。
        // 刻意不用 lint-baseline.xml：基线会把新出现的问题也一起藏起来。
        disable += setOf(
            // 明文 HTTP 是本项目的设计前提（用户手输局域网 IP，无法收敛到某个域名），
            // 由 network_security_config.xml 显式放行；见设计 9。
            "InsecureBaseConfiguration",
            // 依赖版本是刻意钉住的（见 libs.versions.toml 的注释），
            // 升级需要连带回归验证，不在本轮范围内。
            "NewerVersionAvailable",
            // targetSdk 保持 36：compileSdk 必须 ≥37（AAR metadata 要求），
            // 但 targetSdk 会opt-in 新的运行时行为，没有真机验证前不跟进。
            "OldTargetApi",
        )
    }
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.material.icons.extended)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)

    // 导航：连接页 / 浏览页 / 阅读页
    implementation(libs.androidx.navigation.compose)

    // HTTP / JSON
    implementation(libs.retrofit)
    implementation(libs.retrofit.kotlinx.serialization)
    implementation(libs.okhttp)
    implementation(libs.okhttp.logging)
    implementation(libs.kotlinx.serialization.json)

    // 页面图片与封面加载、磁盘缓存、预取
    implementation(libs.coil.compose)
    implementation(libs.coil.network.okhttp)

    // 服务器地址 / 阅读设置 / 每本书进度
    implementation(libs.androidx.datastore.preferences)

    testImplementation(libs.junit)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
    debugImplementation(libs.androidx.compose.ui.tooling)
}
